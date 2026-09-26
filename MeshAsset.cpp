#include "MeshAsset.h"

#include <deki/LogSystem.h>

#include <cstring>

namespace Deki3D
{
namespace
{

/// The vertex layout a given attribute mask describes. Offsets follow the
/// order the compiler writes them in: position, normal, uv, colour, each
/// present only when its bit is set.
VertexLayout LayoutFor(uint16_t attributes, uint16_t stride)
{
    VertexLayout layout;
    layout.stride = stride;
    int16_t offset = 0;

    layout.positionOffset = offset;
    offset += static_cast<int16_t>(sizeof(float) * 3);

    if (attributes & MeshAttribute_Normal)
    {
        layout.normalOffset = offset;
        offset += static_cast<int16_t>(sizeof(float) * 3);
    }
    if (attributes & MeshAttribute_UV)
    {
        layout.uvOffset = offset;
        offset += static_cast<int16_t>(sizeof(float) * 2);
    }
    if (attributes & MeshAttribute_Color)
    {
        layout.colorOffset = offset;
        offset += static_cast<int16_t>(sizeof(uint32_t));
    }
    return layout;
}

/// The exponent of a power of two, or -1 when the value is not one.
int ShiftOf(uint16_t value)
{
    if (value == 0 || (value & (value - 1)) != 0)
        return -1;
    int shift = 0;
    while ((1u << shift) < value)
        ++shift;
    return shift;
}

/// Reads a blob front to back, refusing to run off the end. Every field of
/// the file goes through this, because a mesh is parsed on devices with no
/// memory protection worth the name and a truncated asset has to fail
/// cleanly rather than read whatever happens to follow it.
class Cursor
{
public:
    Cursor(const uint8_t* data, size_t size) : m_Data(data), m_Size(size) {}

    bool Take(void* out, size_t bytes)
    {
        if (bytes > m_Size - m_Offset)
            return false;
        std::memcpy(out, m_Data + m_Offset, bytes);
        m_Offset += bytes;
        return true;
    }

private:
    const uint8_t* m_Data;
    size_t m_Size;
    size_t m_Offset = 0;
};

}  // namespace

bool MeshAsset::LoadFromMemory(const uint8_t* data, size_t size)
{
    Clear();
    if (!data)
        return false;

    Cursor cursor(data, size);

    MeshFileHeader header{};
    if (!cursor.Take(&header, sizeof(header)))
        return false;

    if (std::memcmp(header.magic, "DMSH", 4) != 0)
    {
        DEKI_LOG_WARNING("MeshAsset: not a mesh (bad magic)");
        return false;
    }
    if (header.version < kMeshFileOldestVersion || header.version > kMeshFileVersion)
    {
        DEKI_LOG_WARNING("MeshAsset: version %u, this build reads %u to %u; rebuild the asset",
                         static_cast<unsigned>(header.version), static_cast<unsigned>(kMeshFileOldestVersion),
                         static_cast<unsigned>(kMeshFileVersion));
        return false;
    }
    if (header.vertexCount == 0 || header.indexCount == 0 || header.vertexStride == 0)
        return false;

    const size_t vertexBytes = static_cast<size_t>(header.vertexCount) * header.vertexStride;
    if (!m_Vertices.Allocate(vertexBytes, Deki::Memory::External))
        return Fail("no room for the vertex buffer");
    if (!cursor.Take(m_Vertices.Data(), vertexBytes))
        return Fail("truncated vertex buffer");

    if (!m_Indices.Allocate(header.indexCount, Deki::Memory::External))
        return Fail("no room for the index buffer");
    if (!cursor.Take(m_Indices.Data(), m_Indices.Bytes()))
        return Fail("truncated index buffer");

    if (!m_Submeshes.Allocate(header.submeshCount > 0 ? header.submeshCount : 1, Deki::Memory::Internal))
        return Fail("no room for the submesh table");
    if (header.submeshCount > 0)
    {
        for (uint16_t i = 0; i < header.submeshCount; ++i)
        {
            MeshFileSubmesh onDisk{};
            if (!cursor.Take(&onDisk, sizeof(onDisk)))
                return Fail("truncated submesh table");
            Submesh3D sub;
            sub.firstIndex = onDisk.firstIndex;
            sub.indexCount = onDisk.indexCount;
            sub.materialIndex = onDisk.materialIndex;
            m_Submeshes.Data()[i] = sub;
        }
    }
    else
    {
        Submesh3D all;
        all.firstIndex = 0;
        all.indexCount = header.indexCount;
        all.materialIndex = 0;
        m_Submeshes.Data()[0] = all;
    }

    Deki::Buffer<MeshFileMaterial> fileMaterials;
    if (!fileMaterials.Allocate(header.materialCount, Deki::Memory::Internal))
        return Fail("no room for the material table");
    for (uint16_t i = 0; i < header.materialCount; ++i)
        if (!cursor.Take(&fileMaterials.Data()[i], sizeof(MeshFileMaterial)))
            return Fail("truncated material table");

    Deki::Buffer<MeshFileTexture> fileTextures;
    if (!fileTextures.Allocate(header.textureCount, Deki::Memory::Internal))
        return Fail("no room for the texture table");
    for (uint16_t i = 0; i < header.textureCount; ++i)
    {
        if (header.version >= 4)
        {
            if (!cursor.Take(&fileTextures.Data()[i], sizeof(MeshFileTexture)))
                return Fail("truncated texture table");
        }
        else
        {
            MeshFileTextureV3 old{};
            if (!cursor.Take(&old, sizeof(old)))
                return Fail("truncated texture table");
            fileTextures.Data()[i] = MeshFileTexture{ old.width, old.height, old.byteOffset,
                                               static_cast<uint8_t>(TexelFormat::RGB565), { 0, 0, 0 } };
        }
    }

    if (header.texturePixelBytes > 0)
    {
        if (!m_TexturePixels.Allocate(header.texturePixelBytes, Deki::Memory::External))
            return Fail("no room for the texture pixels");
        if (!cursor.Take(m_TexturePixels.Data(), header.texturePixelBytes))
            return Fail("truncated texture pixels");
    }

    // --- everything is present; now check that it all points somewhere real ---

    for (size_t i = 0; i < m_Indices.Count(); ++i)
    {
        if (m_Indices.Data()[i] >= header.vertexCount)
            return Fail("an index points past the vertex buffer");
    }
    for (size_t i = 0; i < m_Submeshes.Count(); ++i)
    {
        const Submesh3D& sub = m_Submeshes.Data()[i];
        if (static_cast<size_t>(sub.firstIndex) + sub.indexCount > m_Indices.Count())
            return Fail("a submesh runs past the index buffer");
    }

    if (!m_Textures.Allocate(header.textureCount, Deki::Memory::Internal))
        return Fail("no room for the texture table");
    for (uint16_t ti = 0; ti < header.textureCount; ++ti)
    {
        const MeshFileTexture& t = fileTextures.Data()[ti];
        Texture3D view;
        const int wShift = ShiftOf(t.width);
        const int hShift = ShiftOf(t.height);
        const bool knownFormat = t.format <= static_cast<uint8_t>(TexelFormat::ALPHA8);
        const TexelFormat format = knownFormat ? static_cast<TexelFormat>(t.format) : TexelFormat::RGB565;
        const size_t bytes = static_cast<size_t>(t.width) * t.height * TexelBytes(format);
        const bool fits = static_cast<size_t>(t.byteOffset) + bytes <= m_TexturePixels.Count();

        if (knownFormat && wShift >= 0 && hShift >= 0 && bytes > 0 && fits)
        {
            view.pixels = m_TexturePixels.Data() + t.byteOffset;
            view.palette = nullptr;
            view.format = format;
            view.hasAlpha = format == TexelFormat::RGB565 || format == TexelFormat::RGB565A8 ||
                            format == TexelFormat::RGBA8888 || format == TexelFormat::ALPHA8;
            view.width = t.width;
            view.height = t.height;
            view.widthShift = static_cast<uint8_t>(wShift);
            view.heightShift = static_cast<uint8_t>(hShift);
        }
        else
        {
            // Left invalid rather than rejecting the file: one bad texture
            // should cost its own material's texturing, not the whole model.
            DEKI_LOG_WARNING("MeshAsset: texture %ux%u (format %u) at %u is unusable; that material goes untextured",
                             static_cast<unsigned>(t.width), static_cast<unsigned>(t.height),
                             static_cast<unsigned>(t.format), static_cast<unsigned>(t.byteOffset));
        }
        m_Textures.Data()[ti] = view;
    }

    // A model with no materials of its own still draws, with one default.
    if (!m_Materials.Allocate(header.materialCount ? header.materialCount : 1, Deki::Memory::Internal))
        return Fail("no room for the material table");
    m_Materials.Data()[0] = Material3D{};  // Buffer memory is zeroed, not constructed
    for (uint16_t mi = 0; mi < header.materialCount; ++mi)
    {
        const MeshFileMaterial& m = fileMaterials.Data()[mi];
        Material3D material;
        material.tint = m.tint;
        material.doubleSided = (m.flags & MeshMaterial_DoubleSided) != 0;
        material.alphaTest = (m.flags & MeshMaterial_AlphaTest) != 0;
        if (m.textureIndex >= 0 && static_cast<size_t>(m.textureIndex) < m_Textures.Count() &&
            m_Textures.Data()[m.textureIndex].Valid())
        {
            material.texture = &m_Textures.Data()[m.textureIndex];
        }
        m_Materials.Data()[mi] = material;
    }

    for (size_t i = 0; i < m_Submeshes.Count(); ++i)
    {
        if (m_Submeshes.Data()[i].materialIndex >= m_Materials.Count())
            return Fail("a submesh names a material that is not there");
    }

    m_View.layout = LayoutFor(header.attributes, header.vertexStride);
    m_View.vertexCount = header.vertexCount;
    m_View.indexCount = header.indexCount;
    m_View.boundsMin = Deki::Vector3(header.boundsMin[0], header.boundsMin[1], header.boundsMin[2]);
    m_View.boundsMax = Deki::Vector3(header.boundsMax[0], header.boundsMax[1], header.boundsMax[2]);
    m_View.vertices = m_Vertices.Data();
    m_View.indices = m_Indices.Data();
    m_View.submeshes = m_Submeshes.Data();
    m_View.submeshCount = static_cast<uint16_t>(m_Submeshes.Count());
    return true;
}

void MeshAsset::Clear()
{
    m_Vertices.Reset();
    m_Indices.Reset();
    m_Submeshes.Reset();
    m_TexturePixels.Reset();
    m_Textures.Reset();
    m_Materials.Reset();
    m_View = Mesh3D{};
}

bool MeshAsset::Fail(const char* why)
{
    DEKI_LOG_WARNING("MeshAsset: %s", why);
    Clear();
    return false;
}

}  // namespace Deki3D
