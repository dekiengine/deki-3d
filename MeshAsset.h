#pragma once

// The runtime form of a compiled mesh, and the binary it is compiled to.
//
// The editor turns a source model (.obj) into one blob in the project's asset
// cache, keyed by the asset's GUID; the device reads only that blob. Geometry
// is bulk numeric data, so the blob is flat binary, not JSON or MessagePack:
// the arrays are laid out as the rasteriser reads them, and loading is a
// header parse plus a few copies.
//
// The layout is also what a GPU asks for, so a GPU backend can read the same
// file: one interleaved vertex buffer, one index buffer, submeshes that are
// ranges over the indices, and a material table those ranges point into.
//
// On disk, in order:
//   header
//   vertices        vertexCount * vertexStride
//   indices         indexCount * uint16
//   submeshes       submeshCount * MeshFileSubmesh
//   materials       materialCount * MeshFileMaterial
//   textures        textureCount * MeshFileTexture
//   texture pixels  concatenated, each in its own format, addressed by offset

#include "Deki3DAPI.h"
#include "Mesh3D.h"

#include <deki/providers/Buffer.h>

#include <cstdint>

namespace Deki3D
{

constexpr uint16_t kMeshFileVersion = 4;

/// File header. Little-endian, like every target the engine runs on.
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

/// A draw range. Matches Submesh3D but is declared separately, so changing
/// the runtime struct cannot change the file layout.
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
    MeshMaterialDoubleSided = 1 << 0,
    MeshMaterialAlphaTest = 1 << 1,
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

enum MeshAttribute : uint16_t
{
    MeshAttributePosition = 1 << 0,  // always set
    MeshAttributeNormal = 1 << 1,
    MeshAttributeUV = 1 << 2,
    MeshAttributeColor = 1 << 3,
};

/// A loaded mesh. Owns its buffers. `View()` gives the rasteriser a
/// non-owning Mesh3D over them, and `Materials()` the array its submeshes
/// index into.
class DEKI_3D_API MeshAsset
{
public:
    /// What AssetRef<MeshAsset> asks the AssetManager for.
    static constexpr const char* kAssetTypeName = "Mesh";

    /// Parses a compiled blob. Returns false and leaves the asset empty on a
    /// bad magic, an unknown version, or anything that does not fit.
    bool LoadFromMemory(const uint8_t* data, size_t size);

    const Mesh3D& View() const { return m_View; }
    bool Valid() const { return m_View.vertexCount > 0 && m_View.indexCount > 0; }

    /// One entry per material the submeshes index, with its texture already
    /// resolved. Never empty for a valid mesh: a model with no materials of
    /// its own gets one plain white default.
    const Material3D* Materials() const { return m_Materials.Data(); }
    uint16_t MaterialCount() const { return static_cast<uint16_t>(m_Materials.Count()); }

    uint32_t VertexCount() const { return m_View.vertexCount; }
    uint32_t TriangleCount() const { return m_View.indexCount / 3; }
    uint16_t TextureCount() const { return static_cast<uint16_t>(m_Textures.Count()); }

private:
    void Clear();
    /// Logs why, empties everything, and returns false. A half-parsed mesh
    /// must never be left reachable.
    bool Fail(const char* why);

    // Engine memory, sized by the file: a size that does not fit fails the
    // load with a log line instead of aborting the board. The big buffers
    // (vertices, indices, texture pixels) are External, which is PSRAM where
    // the board has it and the one heap where it does not; the small tables
    // are Internal.
    Deki::Buffer<uint8_t> m_Vertices;
    Deki::Buffer<uint16_t> m_Indices;
    Deki::Buffer<Submesh3D> m_Submeshes;
    Deki::Buffer<uint8_t> m_TexturePixels;
    Deki::Buffer<Texture3D> m_Textures;
    Deki::Buffer<Material3D> m_Materials;
    Mesh3D m_View;
};

/// Registers the "Mesh" loader with the engine's AssetManager. Called from a
/// static initialiser in MeshAssetLoader.cpp and from Deki3DInitSystem();
/// safe to call more than once.
DEKI_3D_API void Deki3DRegisterMeshLoader();

}  // namespace Deki3D
