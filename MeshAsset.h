#pragma once

/**
 * @file MeshAsset.h
 * @brief The runtime form of a compiled mesh, and the binary it is compiled to.
 *
 * The editor turns a source model (.obj today) into one blob in the project's
 * asset cache, keyed by the asset's GUID; the device reads that blob and
 * nothing else. Geometry is bulk numeric data, so the compiled form is a flat
 * binary rather than JSON or MessagePack: the arrays are laid out exactly as
 * the rasteriser wants to read them, and loading is a header parse plus a few
 * copies.
 *
 * The layout is deliberately the one a GPU would ask for too, so a later
 * backend consumes the same file: one interleaved vertex buffer, one index
 * buffer, submeshes that are ranges over the indices, and a material table
 * those ranges point into.
 *
 * On disk, in order:
 *   header
 *   vertices        vertexCount * vertexStride
 *   indices         indexCount * uint16
 *   submeshes       submeshCount * MeshFileSubmesh
 *   materials       materialCount * MeshFileMaterial
 *   textures        textureCount * MeshFileTexture
 *   texture pixels  concatenated, each in its own format, addressed by offset
 *
 * Version 3 files have no format in the texture table (MeshFileTextureV3) and
 * every texture is RGB565; they still load.
 */

#include "Deki3DAPI.h"
#include "Mesh3D.h"

#include <cstdint>
#include <vector>

namespace Deki3D
{

constexpr uint16_t kMeshFileVersion = 4;
constexpr uint16_t kMeshFileOldestVersion = 3;

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
    uint16_t materialCount;
    uint16_t textureCount;
    uint32_t texturePixelBytes;  // total size of the pixel blob that follows
    float boundsMin[3];
    float boundsMax[3];
};

/// A draw range. Matches Submesh3D, but spelled separately so the on-disk
/// layout cannot drift when the runtime struct is rearranged.
struct MeshFileSubmesh
{
    uint32_t firstIndex;
    uint32_t indexCount;
    uint16_t materialIndex;
    uint16_t padding;
};

struct MeshFileMaterial
{
    int16_t textureIndex;  // -1 when the material has no texture
    uint16_t flags;        // MeshMaterialFlags
    uint32_t tint;         // 0xAABBGGRR
};

enum MeshMaterialFlags : uint16_t
{
    MeshMaterial_DoubleSided = 1 << 0,
    MeshMaterial_AlphaTest = 1 << 1,
};

/// Where one texture lives in the pixel blob. Both sides are powers of two so
/// the sampler masks the coordinate instead of dividing.
struct MeshFileTexture
{
    uint16_t width;
    uint16_t height;
    uint32_t byteOffset;  // from the start of the pixel blob
    uint8_t format;       // TexelFormat
    uint8_t padding[3];
};

/// The version 3 texture record: RGB565, no format byte.
struct MeshFileTextureV3
{
    uint16_t width;
    uint16_t height;
    uint32_t byteOffset;
};

enum MeshAttribute : uint16_t
{
    MeshAttribute_Position = 1 << 0,  // always set
    MeshAttribute_Normal = 1 << 1,
    MeshAttribute_UV = 1 << 2,
    MeshAttribute_Color = 1 << 3,
};

/// A loaded mesh. Owns its buffers; `View()` hands the rasteriser a
/// non-owning Mesh3D over them, and `Materials()` the array its submeshes
/// index into.
class DEKI_3D_API MeshAsset
{
public:
    /// What AssetRef<MeshAsset> asks the AssetManager for.
    static constexpr const char* AssetTypeName = "Mesh";

    /// Parse a compiled blob. Returns false and leaves the asset empty on a
    /// bad magic, an unknown version, or anything that does not fit.
    bool LoadFromMemory(const uint8_t* data, size_t size);

    const Mesh3D& View() const { return m_View; }
    bool Valid() const { return m_View.vertexCount > 0 && m_View.indexCount > 0; }

    /// One entry per material the submeshes index, with its texture already
    /// resolved. Never empty for a valid mesh: a model with no materials of
    /// its own gets one plain white default.
    const Material3D* Materials() const { return m_Materials.data(); }
    uint16_t MaterialCount() const { return static_cast<uint16_t>(m_Materials.size()); }

    uint32_t VertexCount() const { return m_View.vertexCount; }
    uint32_t TriangleCount() const { return m_View.indexCount / 3; }
    uint16_t TextureCount() const { return static_cast<uint16_t>(m_Textures.size()); }

private:
    void Clear();
    /// Log why, empty everything, and return false. A half-parsed mesh must
    /// never be left reachable.
    bool Fail(const char* why);

    std::vector<uint8_t> m_Vertices;
    std::vector<uint16_t> m_Indices;
    std::vector<Submesh3D> m_Submeshes;
    std::vector<uint8_t> m_TexturePixels;
    std::vector<Texture3D> m_Textures;
    std::vector<Material3D> m_Materials;
    Mesh3D m_View;
};

/// Register the "Mesh" loader with the engine's AssetManager. Called from a
/// static initialiser in MeshAssetLoader.cpp; safe to call more than once.
DEKI_3D_API void Deki3D_RegisterMeshLoader();

}  // namespace Deki3D
