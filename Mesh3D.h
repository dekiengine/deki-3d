#pragma once

/**
 * @file Mesh3D.h
 * @brief Geometry as a GPU would want it: a vertex buffer, an index buffer,
 *        and draw ranges.
 *
 * The shape here is deliberate. A software rasterizer would be happy with
 * arrays of structs and pointers between them; a GPU backend wants one
 * contiguous vertex buffer with a declared attribute layout, one index
 * buffer, and submeshes that are nothing more than (first index, count,
 * material) ranges. Since only the rasterizer is throwaway when a GPU
 * backend arrives, the data it consumes is written the GPU's way from the
 * start, and the span loop pays a little indirection at vertex rate for it.
 *
 * Attributes are float here. Quantising positions or dropping normals for a
 * small target is a decision the asset compiler makes per build, expressed
 * as a different VertexLayout over the same buffer, not a different runtime
 * type.
 */

#include <deki/Vector.h>

#include <cstddef>
#include <cstdint>

namespace Deki3D
{

/// Byte offsets of each attribute within a vertex, -1 when the buffer does
/// not carry it. Mirrors a vertex attribute description in a graphics API.
struct VertexLayout
{
    uint16_t stride = 0;
    int16_t positionOffset = -1;  // 3 x float, required
    int16_t normalOffset = -1;    // 3 x float
    int16_t uvOffset = -1;        // 2 x float
    int16_t colorOffset = -1;     // uint32, 0xAABBGGRR

    bool HasNormals() const { return normalOffset >= 0; }
    bool HasUVs() const { return uvOffset >= 0; }
    bool HasColors() const { return colorOffset >= 0; }
};

/// Position, normal, uv, colour. The layout a mesh gets when nothing smaller
/// was asked for.
struct VertexPNTC
{
    Deki::Vector3 position;
    Deki::Vector3 normal;
    float u, v;
    uint32_t color;
};

inline VertexLayout LayoutPNTC()
{
    VertexLayout l;
    l.stride = static_cast<uint16_t>(sizeof(VertexPNTC));
    l.positionOffset = static_cast<int16_t>(offsetof(VertexPNTC, position));
    l.normalOffset = static_cast<int16_t>(offsetof(VertexPNTC, normal));
    l.uvOffset = static_cast<int16_t>(offsetof(VertexPNTC, u));
    l.colorOffset = static_cast<int16_t>(offsetof(VertexPNTC, color));
    return l;
}

/// How a submesh is shaded. The portable set: every backend implements all of
/// these, so content using them runs anywhere. A custom shader is a GPU-tier
/// thing that sits above this enum, not inside it.
enum class ShadingModel : uint8_t
{
    Unlit = 0,      // texture and/or vertex colour, no lighting
    VertexLit = 1,  // lambert against a single directional light, per vertex
    Flat = 2,       // lambert per triangle from the face normal
};

/// A texture the rasterizer can sample: 8-bit indices into a palette, or
/// RGB565 direct. Dimensions are powers of two so the sampler masks instead
/// of dividing.
struct Texture3D
{
    const uint8_t* pixels = nullptr;
    const uint16_t* palette = nullptr;  // non-null => `pixels` are indices
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t widthShift = 0;   // width == 1 << widthShift
    uint8_t heightShift = 0;  // height == 1 << heightShift
    bool hasAlpha = false;    // palette index 0 / RGB565 magenta is a hole

    bool Valid() const { return pixels != nullptr && width > 0 && height > 0; }
};

struct Material3D
{
    const Texture3D* texture = nullptr;
    uint32_t tint = 0xFFFFFFFFu;  // 0xAABBGGRR, modulates the sampled colour
    ShadingModel shading = ShadingModel::Unlit;
    bool doubleSided = false;
    bool alphaTest = false;  // discard texels the texture calls holes
};

/// A range of the index buffer drawn with one material, the software echo of
/// a single draw call.
struct Submesh3D
{
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint16_t materialIndex = 0;
};

/// The runtime form of a .mesh asset. Buffers are owned elsewhere (the asset
/// loader), so this is a view and stays trivially copyable.
struct Mesh3D
{
    const void* vertices = nullptr;
    uint32_t vertexCount = 0;
    VertexLayout layout;

    const uint16_t* indices = nullptr;  // 16-bit: 65535 vertices per mesh
    uint32_t indexCount = 0;

    const Submesh3D* submeshes = nullptr;
    uint16_t submeshCount = 0;

    // Object-space bounds, for culling before anything is transformed.
    Deki::Vector3 boundsMin;
    Deki::Vector3 boundsMax;

    const void* VertexAt(uint32_t index) const
    {
        return static_cast<const uint8_t*>(vertices) + static_cast<size_t>(index) * layout.stride;
    }
};

}  // namespace Deki3D
