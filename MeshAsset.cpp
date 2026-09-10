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

}  // namespace

bool MeshAsset::LoadFromMemory(const uint8_t* data, size_t size)
{
    m_Vertices.clear();
    m_Indices.clear();
    m_Submeshes.clear();
    m_TexturePixels.clear();
    m_Texture = Texture3D{};
    m_View = Mesh3D{};

    if (!data || size < sizeof(MeshFileHeader))
        return false;

    MeshFileHeader header{};
    std::memcpy(&header, data, sizeof(header));

    if (std::memcmp(header.magic, "DMSH", 4) != 0)
    {
        DEKI_LOG_WARNING("MeshAsset: not a mesh (bad magic)");
        return false;
    }
    if (header.version != kMeshFileVersion)
    {
        DEKI_LOG_WARNING("MeshAsset: version %u, this build reads %u; rebuild the asset",
                         static_cast<unsigned>(header.version),
                         static_cast<unsigned>(kMeshFileVersion));
        return false;
    }
    if (header.vertexCount == 0 || header.indexCount == 0 || header.vertexStride == 0)
        return false;

    // Every offset is checked against the file's real length before anything
    // is copied: a truncated or hand-edited asset must not read past the end.
    const size_t vertexBytes = static_cast<size_t>(header.vertexCount) * header.vertexStride;
    const size_t indexBytes = static_cast<size_t>(header.indexCount) * sizeof(uint16_t);
    const size_t submeshBytes = static_cast<size_t>(header.submeshCount) * sizeof(Submesh3D);
    const size_t textureBytes =
        static_cast<size_t>(header.textureWidth) * header.textureHeight * sizeof(uint16_t);
    const size_t needed =
        sizeof(MeshFileHeader) + vertexBytes + indexBytes + submeshBytes + textureBytes;
    if (size < needed)
    {
        DEKI_LOG_WARNING("MeshAsset: truncated (%zu bytes, needs %zu)", size, needed);
        return false;
    }

    const uint8_t* cursor = data + sizeof(MeshFileHeader);

    m_Vertices.resize(vertexBytes);
    std::memcpy(m_Vertices.data(), cursor, vertexBytes);
    cursor += vertexBytes;

    m_Indices.resize(header.indexCount);
    std::memcpy(m_Indices.data(), cursor, indexBytes);
    cursor += indexBytes;

    if (header.submeshCount > 0)
    {
        m_Submeshes.resize(header.submeshCount);
        std::memcpy(m_Submeshes.data(), cursor, submeshBytes);
        cursor += submeshBytes;
    }
    else
    {
        // A mesh with no submesh table is one range over everything.
        Submesh3D all;
        all.firstIndex = 0;
        all.indexCount = header.indexCount;
        all.materialIndex = 0;
        m_Submeshes.push_back(all);
    }

    // An index past the end of the vertex buffer would have the rasteriser
    // read arbitrary memory, so it is rejected here rather than trusted.
    for (uint16_t index : m_Indices)
    {
        if (index >= header.vertexCount)
        {
            DEKI_LOG_WARNING("MeshAsset: index %u is past the %u vertices it has",
                             static_cast<unsigned>(index), header.vertexCount);
            m_Vertices.clear();
            m_Indices.clear();
            m_Submeshes.clear();
            return false;
        }
    }
    for (const Submesh3D& sub : m_Submeshes)
    {
        if (static_cast<size_t>(sub.firstIndex) + sub.indexCount > m_Indices.size())
        {
            DEKI_LOG_WARNING("MeshAsset: a submesh runs past the index buffer");
            m_Vertices.clear();
            m_Indices.clear();
            m_Submeshes.clear();
            return false;
        }
    }

    if (textureBytes > 0)
    {
        // Powers of two only: the sampler masks the coordinate rather than
        // wrapping it with a division, so anything else would read the wrong
        // texels rather than merely look wrong.
        const int wShift = ShiftOf(header.textureWidth);
        const int hShift = ShiftOf(header.textureHeight);
        if (wShift < 0 || hShift < 0)
        {
            DEKI_LOG_WARNING("MeshAsset: texture %ux%u is not a power of two; ignoring it",
                             static_cast<unsigned>(header.textureWidth),
                             static_cast<unsigned>(header.textureHeight));
        }
        else
        {
            m_TexturePixels.resize(textureBytes);
            std::memcpy(m_TexturePixels.data(), cursor, textureBytes);
            m_Texture.pixels = m_TexturePixels.data();
            m_Texture.palette = nullptr;
            m_Texture.width = header.textureWidth;
            m_Texture.height = header.textureHeight;
            m_Texture.widthShift = static_cast<uint8_t>(wShift);
            m_Texture.heightShift = static_cast<uint8_t>(hShift);
        }
    }

    m_View.layout = LayoutFor(header.attributes, header.vertexStride);
    m_View.vertexCount = header.vertexCount;
    m_View.indexCount = header.indexCount;
    m_View.boundsMin = Deki::Vector3(header.boundsMin[0], header.boundsMin[1], header.boundsMin[2]);
    m_View.boundsMax = Deki::Vector3(header.boundsMax[0], header.boundsMax[1], header.boundsMax[2]);
    RebuildView();
    return true;
}

void MeshAsset::RebuildView()
{
    m_View.vertices = m_Vertices.data();
    m_View.indices = m_Indices.data();
    m_View.submeshes = m_Submeshes.data();
    m_View.submeshCount = static_cast<uint16_t>(m_Submeshes.size());
}

}  // namespace Deki3D
