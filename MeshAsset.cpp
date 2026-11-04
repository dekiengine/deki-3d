#include "MeshAsset.h"

#include <deki/LogSystem.h>
#include <deki/providers/FileSystem.h>
#include <deki/providers/IFileSystem.h>

#include "Mesh3DBackend.h"  // NextRevision
#include "deki-rendering/PixelFormat.h"

#include <cstdint>
#include <cstring>

namespace Deki3D
{
namespace
{

/// The vertex layout an attribute mask describes. Offsets follow the order
/// the asset compiler writes: position, normal, uv, colour, each present only
/// when its bit is set.
VertexLayout LayoutFor(uint16_t attributes, uint16_t stride)
{
    VertexLayout layout;
    layout.stride = stride;
    int16_t offset = 0;

    layout.positionOffset = offset;
    offset += static_cast<int16_t>(sizeof(float) * 3);

    if (attributes & MeshAttributeNormal)
    {
        layout.normalOffset = offset;
        offset += static_cast<int16_t>(sizeof(float) * 3);
    }
    if (attributes & MeshAttributeUV)
    {
        layout.uvOffset = offset;
        offset += static_cast<int16_t>(sizeof(float) * 2);
    }
    if (attributes & MeshAttributeColor)
    {
        layout.colorOffset = offset;
        offset += static_cast<int16_t>(sizeof(uint32_t));
    }
    return layout;
}

/// The bytes one vertex with these attributes needs (LayoutFor's offsets).
uint32_t VertexBytesFor(uint16_t attributes)
{
    uint32_t bytes = sizeof(float) * 3;
    if (attributes & MeshAttributeNormal)
    {
        bytes += sizeof(float) * 3;
    }
    if (attributes & MeshAttributeUV)
    {
        bytes += sizeof(float) * 2;
    }
    if (attributes & MeshAttributeColor)
    {
        bytes += sizeof(uint32_t);
    }
    return bytes;
}

/// The exponent of a power of two, or -1 when the value is not one.
int ShiftOf(uint16_t value)
{
    if (value == 0 || (value & (value - 1)) != 0)
    {
        return -1;
    }
    int shift = 0;
    while ((1u << shift) < value)
    {
        ++shift;
    }
    return shift;
}

}  // namespace

/// Where a load reads the file from, front to back. Every field goes through
/// Take, which refuses to run off the end: devices have little memory
/// protection, so a truncated asset must fail cleanly instead of reading
/// whatever follows it.
class MeshSource
{
public:
    virtual ~MeshSource() = default;
    virtual bool Take(void* out, size_t bytes) = 0;
};

namespace
{

/// A blob already in memory.
class MemorySource : public MeshSource
{
public:
    MemorySource(const uint8_t* data, size_t size)
        : m_Data(data),
          m_Size(size)
    {
    }

    bool Take(void* out, size_t bytes) override
    {
        if (bytes > m_Size - m_Offset)
        {
            return false;
        }
        std::memcpy(out, m_Data + m_Offset, bytes);
        m_Offset += bytes;
        return true;
    }

private:
    const uint8_t* m_Data;
    size_t m_Size;
    size_t m_Offset = 0;
};

/// A file, read a part at a time into wherever the part belongs.
class FileSource : public MeshSource
{
public:
    explicit FileSource(const char* path)
        : m_Fs(Deki::FileSystem::GetFileSystemForPath(path))
    {
        if (m_Fs)
        {
            m_File = m_Fs->OpenFile(path, Deki::IFileSystem::OpenMode::ReadBinary);
        }
        if (m_File)
        {
            const long size = m_Fs->GetFileSize(m_File);
            m_Left = size > 0 ? static_cast<size_t>(size) : 0;
        }
    }

    ~FileSource() override
    {
        if (m_File)
        {
            m_Fs->CloseFile(m_File);
        }
    }

    bool Open() const { return m_File != nullptr; }

    bool Take(void* out, size_t bytes) override
    {
        if (!m_File || bytes > m_Left)
        {
            return false;
        }
        if (bytes > 0 && m_Fs->ReadFile(m_File, out, bytes) != bytes)
        {
            return false;
        }
        m_Left -= bytes;
        return true;
    }

private:
    Deki::IFileSystem* m_Fs;
    Deki::IFileSystem::FileHandle m_File = nullptr;
    size_t m_Left = 0;
};

}  // namespace

bool MeshAsset::LoadFromMemory(const uint8_t* data, size_t size)
{
    Clear();
    if (!data)
    {
        return false;
    }
    MemorySource source(data, size);
    return Load(source);
}

bool MeshAsset::LoadFromFile(const char* path)
{
    Clear();
    FileSource source(path);
    if (!source.Open())
    {
        DEKI_LOG_WARNING("MeshAsset: cannot open %s", path ? path : "(null)");
        return false;
    }
    return Load(source);
}

bool MeshAsset::Load(MeshSource& cursor)
{

    MeshFileHeader header{};
    if (!cursor.Take(&header, kMeshFileHeaderV4Bytes))
    {
        return false;
    }

    if (std::memcmp(header.magic, "DMSH", 4) != 0)
    {
        DEKI_LOG_WARNING("MeshAsset: not a mesh (bad magic)");
        return false;
    }
    if (header.version != kMeshFileVersion && header.version != 4)
    {
        DEKI_LOG_WARNING("MeshAsset: version %u, this build reads 4 and %u; rebuild the asset",
                         static_cast<unsigned>(header.version), static_cast<unsigned>(kMeshFileVersion));
        return false;
    }
    header.flags = 0;
    if (header.version >= 5 && !cursor.Take(&header.flags, sizeof(header.flags)))
    {
        return false;
    }
    const uint8_t indexBytes = (header.flags & MeshFileIndex32) ? 4 : 2;
    if (header.vertexCount == 0 || header.indexCount == 0 || header.vertexStride == 0)
    {
        return false;
    }
    // The stride must hold the attributes it claims and keep its floats 4-byte
    // aligned: a short one reads past the buffer, and unaligned float loads
    // trap on Xtensa.
    if (header.vertexStride < VertexBytesFor(header.attributes) || header.vertexStride % 4 != 0)
    {
        return Fail("its vertex stride does not fit its attributes");
    }

    // In 64 bits: vertexCount * stride can wrap on a 32-bit board, giving a
    // small buffer that indices then read past.
    const uint64_t vertexBytes64 = static_cast<uint64_t>(header.vertexCount) * header.vertexStride;
    if (vertexBytes64 > SIZE_MAX / 2)
    {
        return Fail("its vertex buffer is larger than this device can address");
    }
    const size_t vertexBytes = static_cast<size_t>(vertexBytes64);
    if (!m_Vertices.Allocate(vertexBytes, Deki::Memory::External))
    {
        return Fail("no room for the vertex buffer");
    }
    if (!cursor.Take(m_Vertices.Data(), vertexBytes))
    {
        return Fail("truncated vertex buffer");
    }

    if (static_cast<uint64_t>(header.indexCount) * indexBytes > SIZE_MAX / 2)
    {
        return Fail("its index buffer is larger than this device can address");
    }
    if (!m_Indices.Allocate(static_cast<size_t>(header.indexCount) * indexBytes, Deki::Memory::External))
    {
        return Fail("no room for the index buffer");
    }
    if (!cursor.Take(m_Indices.Data(), m_Indices.Bytes()))
    {
        return Fail("truncated index buffer");
    }

    if (!m_Submeshes.Allocate(header.submeshCount > 0 ? header.submeshCount : 1, Deki::Memory::Internal))
    {
        return Fail("no room for the submesh table");
    }
    if (header.submeshCount > 0)
    {
        for (uint16_t i = 0; i < header.submeshCount; ++i)
        {
            MeshFileSubmesh onDisk{};
            if (!cursor.Take(&onDisk, sizeof(onDisk)))
            {
                return Fail("truncated submesh table");
            }
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
    {
        return Fail("no room for the material table");
    }
    for (uint16_t i = 0; i < header.materialCount; ++i)
    {
        if (!cursor.Take(&fileMaterials.Data()[i], sizeof(MeshFileMaterial)))
        {
            return Fail("truncated material table");
        }
    }

    Deki::Buffer<MeshFileTexture> fileTextures;
    if (!fileTextures.Allocate(header.textureCount, Deki::Memory::Internal))
    {
        return Fail("no room for the texture table");
    }
    for (uint16_t i = 0; i < header.textureCount; ++i)
    {
        if (!cursor.Take(&fileTextures.Data()[i], sizeof(MeshFileTexture)))
        {
            return Fail("truncated texture table");
        }
    }

    if (header.texturePixelBytes > 0)
    {
        if (!m_TexturePixels.Allocate(header.texturePixelBytes, Deki::Memory::External))
        {
            return Fail("no room for the texture pixels");
        }
        if (!cursor.Take(m_TexturePixels.Data(), header.texturePixelBytes))
        {
            return Fail("truncated texture pixels");
        }
    }

    // Everything is read; now check that every reference points inside it.

    m_View.indices = m_Indices.Data();
    m_View.indexBytes = indexBytes;
    for (uint32_t i = 0; i < header.indexCount; ++i)
    {
        if (m_View.IndexAt(i) >= header.vertexCount)
        {
            return Fail("an index points past the vertex buffer");
        }
    }
    for (size_t i = 0; i < m_Submeshes.Count(); ++i)
    {
        const Submesh3D& sub = m_Submeshes.Data()[i];
        if (static_cast<uint64_t>(sub.firstIndex) + sub.indexCount > header.indexCount)
        {
            return Fail("a submesh runs past the index buffer");
        }
    }

    if (!m_Textures.Allocate(header.textureCount, Deki::Memory::Internal))
    {
        return Fail("no room for the texture table");
    }
    for (uint16_t ti = 0; ti < header.textureCount; ++ti)
    {
        const MeshFileTexture& t = fileTextures.Data()[ti];
        Texture3D view;
        const int wShift = ShiftOf(t.width);
        const int hShift = ShiftOf(t.height);
        const bool knownFormat = t.format <= static_cast<uint8_t>(TexelFormat::ALPHA8);
        const TexelFormat format = knownFormat ? static_cast<TexelFormat>(t.format) : TexelFormat::RGB565;
        const uint64_t bytes = static_cast<uint64_t>(t.width) * t.height * TexelBytes(format);
        // 64-bit, so offset + size cannot wrap past the check; and aligned for
        // the 2- and 4-byte texel reads (Raster3D reads through uint16_t*).
        const uint64_t texel = TexelBytes(format);
        const bool aligned = (texel == 2 || texel == 4) ? (t.byteOffset % texel) == 0 : true;
        const bool fits = aligned && static_cast<uint64_t>(t.byteOffset) + bytes <= m_TexturePixels.Count();

        if (knownFormat && wShift >= 0 && hShift >= 0 && bytes > 0 && fits)
        {
            // The file keeps RGB565 low byte first; draw from memory's order,
            // high byte first on a big-endian board. Once per pixel run, even
            // if two textures share it: a second swap would undo the first.
            if (format == TexelFormat::RGB565 || format == TexelFormat::RGB565A8)
            {
                bool converted = false;
                for (uint16_t prev = 0; prev < ti && !converted; ++prev)
                {
                    const MeshFileTexture& p = fileTextures.Data()[prev];
                    converted = p.byteOffset == t.byteOffset && m_Textures.Data()[prev].Valid() &&
                                (m_Textures.Data()[prev].format == TexelFormat::RGB565 ||
                                 m_Textures.Data()[prev].format == TexelFormat::RGB565A8);
                }
                if (!converted)
                {
                    DekiPixel::FileToMemory565(m_TexturePixels.Data() + t.byteOffset,
                                               static_cast<size_t>(t.width) * t.height, static_cast<size_t>(texel));
                }
            }
            view.pixels = m_TexturePixels.Data() + t.byteOffset;
            view.palette = nullptr;
            view.format = format;
            view.hasAlpha = format == TexelFormat::RGB565 || format == TexelFormat::RGB565A8 ||
                            format == TexelFormat::RGBA8888 || format == TexelFormat::ALPHA8;
            const uint8_t filter = t.flags & MeshTextureFilterMask;
            view.filter = filter <= static_cast<uint8_t>(Deki::TextureFilter::Trilinear)
                              ? static_cast<Deki::TextureFilter>(filter)
                              : Deki::TextureFilter::Point;
            view.width = t.width;
            view.height = t.height;
            view.widthShift = static_cast<uint8_t>(wShift);
            view.heightShift = static_cast<uint8_t>(hShift);
        }
        else
        {
            // Left invalid instead of rejecting the file: one bad texture
            // costs only its material's texturing, not the whole model.
            DEKI_LOG_WARNING("MeshAsset: texture %ux%u (format %u) at %u is unusable; that material goes untextured",
                             static_cast<unsigned>(t.width), static_cast<unsigned>(t.height),
                             static_cast<unsigned>(t.format), static_cast<unsigned>(t.byteOffset));
        }
        if (view.Valid())
        {
            view.revision = NextRevision();
        }
        m_Textures.Data()[ti] = view;
    }

    // A model with no materials of its own still draws, with one default.
    if (!m_Materials.Allocate(header.materialCount ? header.materialCount : 1, Deki::Memory::Internal))
    {
        return Fail("no room for the material table");
    }
    m_Materials.Data()[0] = Material3D{};  // Buffer memory is zeroed, not constructed
    for (uint16_t mi = 0; mi < header.materialCount; ++mi)
    {
        const MeshFileMaterial& m = fileMaterials.Data()[mi];
        Material3D material;
        material.tint = m.tint;
        material.doubleSided = (m.flags & MeshMaterialDoubleSided) != 0;
        material.alphaTest = (m.flags & MeshMaterialAlphaTest) != 0;
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
        {
            return Fail("a submesh names a material that is not there");
        }
    }

    m_View.layout = LayoutFor(header.attributes, header.vertexStride);
    m_View.vertexCount = header.vertexCount;
    m_View.indexCount = header.indexCount;
    m_View.boundsMin = Deki::Vector3(header.boundsMin[0], header.boundsMin[1], header.boundsMin[2]);
    m_View.boundsMax = Deki::Vector3(header.boundsMax[0], header.boundsMax[1], header.boundsMax[2]);
    m_View.vertices = m_Vertices.Data();
    m_View.submeshes = m_Submeshes.Data();
    m_View.submeshCount = static_cast<uint16_t>(m_Submeshes.Count());
    m_View.revision = NextRevision();
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
