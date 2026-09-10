#pragma once

/**
 * @file MeshAsset.h
 * @brief The runtime form of a compiled mesh, and the binary it is compiled to.
 *
 * The editor turns a source model (.obj today) into one blob in the project's
 * asset cache, keyed by the asset's GUID; the device reads that blob and
 * nothing else. Geometry is bulk numeric data, so the compiled form is a flat
 * binary rather than JSON or MessagePack: the arrays are laid out exactly as
 * the rasteriser wants to read them, and loading is a header parse plus three
 * copies.
 *
 * The layout is deliberately the one a GPU would ask for too, so a later
 * backend consumes the same file: one interleaved vertex buffer, one index
 * buffer, and submeshes that are ranges over the indices.
 */

#include "Deki3DAPI.h"
#include "Mesh3D.h"

#include <cstdint>
#include <vector>

namespace Deki3D
{

/// File header. Little-endian, which every target this engine runs on is.
struct MeshFileHeader
{
    char magic[4];        // "DMSH"
    uint16_t version;     // kMeshFileVersion
    uint16_t attributes;  // MeshAttribute bits; position is always present
    uint32_t vertexCount;
    uint32_t indexCount;
    uint16_t submeshCount;
    uint16_t vertexStride;
    float boundsMin[3];
    float boundsMax[3];
    // An optional texture, RGB565, stored after the submesh table. Both sides
    // are powers of two so the sampler masks instead of dividing. Zero means
    // the mesh carries none and draws with its vertex colours.
    uint16_t textureWidth;
    uint16_t textureHeight;
};

constexpr uint16_t kMeshFileVersion = 2;

enum MeshAttribute : uint16_t
{
    MeshAttribute_Position = 1 << 0,  // always set
    MeshAttribute_Normal = 1 << 1,
    MeshAttribute_UV = 1 << 2,
    MeshAttribute_Color = 1 << 3,
};

/// A loaded mesh. Owns its buffers; `View()` hands the rasteriser a
/// non-owning Mesh3D over them.
class DEKI_3D_API MeshAsset
{
public:
    /// What AssetRef<MeshAsset> asks the AssetManager for.
    static constexpr const char* AssetTypeName = "Mesh";

    /// Parse a compiled blob. Returns false and leaves the asset empty on a
    /// bad magic, an unknown version, or a truncated file.
    bool LoadFromMemory(const uint8_t* data, size_t size);

    const Mesh3D& View() const { return m_View; }
    bool Valid() const { return m_View.vertexCount > 0 && m_View.indexCount > 0; }

    /// The mesh's own texture, or null when it has none.
    const Texture3D* Texture() const { return m_Texture.Valid() ? &m_Texture : nullptr; }

    uint32_t VertexCount() const { return m_View.vertexCount; }
    uint32_t TriangleCount() const { return m_View.indexCount / 3; }

private:
    void RebuildView();

    std::vector<uint8_t> m_Vertices;
    std::vector<uint16_t> m_Indices;
    std::vector<Submesh3D> m_Submeshes;
    std::vector<uint8_t> m_TexturePixels;
    Texture3D m_Texture;
    Mesh3D m_View;
};

/// Register the "Mesh" loader with the engine's AssetManager. Called from the
/// package entry point; safe to call more than once.
DEKI_3D_API void Deki3D_RegisterMeshLoader();

}  // namespace Deki3D
