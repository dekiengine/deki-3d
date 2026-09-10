#pragma once

/**
 * @file Raster3D.h
 * @brief Tiled software triangle rasteriser.
 *
 * Draws into whatever framebuffer it is handed, so it depends on the engine
 * for nothing but its maths types and its colour format enum. That is what
 * lets it be benchmarked and unit tested on its own, away from a scene, a
 * camera or an asset.
 *
 * How a frame works:
 *   BeginFrame  target, size, format, configuration
 *   DrawMesh    per mesh: transform, near-clip, project, bin into tiles
 *   EndFrame    per tile: clear its depth, fill the triangles binned to it
 *
 * Binning first and filling per tile is what keeps the depth buffer small.
 * A full-screen 16-bit depth buffer at 320x240 is 150 KB, which a plain
 * ESP32 cannot spare; one 32x32 tile is 2 KB and gets reused across the
 * whole frame. The same arrangement is a win on a desktop, where the tile's
 * depth and colour working set stays inside L1.
 *
 * Float does the per-vertex work: transform, clipping, projection, and the
 * perspective divide at span endpoints. Fixed point does the per-pixel work:
 * edge functions, depth, and attribute stepping. That split is not a
 * concession to small hardware, it is what a span loop wants everywhere, and
 * it means the parts without a hardware FPU only pay emulation at vertex
 * rate.
 */

#include <deki/Engine.h>  // Deki::ColorFormat
#include <deki/Vector.h>

#include "Mesh3D.h"

#include <cstdint>
#include <vector>

namespace Deki3D
{

struct RasterConfig
{
    /// Tile edge in pixels. The per-tile depth buffer is tileSize^2 * 2 bytes.
    int tileSize = 32;

    /// Divide each span and do the perspective divide at the subdivision
    /// points, interpolating linearly between. The divide then costs once per
    /// `spanSubdivision` pixels instead of once per pixel, which is what makes
    /// correct texturing nearly as cheap as affine.
    bool perspectiveCorrect = true;
    int spanSubdivision = 16;

    /// Round projected vertices to whole pixels. The PlayStation did this
    /// because its transform was integer; here it is a look you ask for.
    bool vertexSnap = false;

    bool backfaceCull = true;
    bool depthTest = true;
    bool depthWrite = true;

    /// Direction the single directional light travels, for the lit shading
    /// models. Normalised by the rasteriser.
    Deki::Vector3 lightDirection = Deki::Vector3(-0.4f, -0.8f, -0.45f);
    float ambient = 0.25f;
};

struct RasterStats
{
    uint32_t trianglesIn = 0;       // handed to DrawMesh
    uint32_t trianglesClipped = 0;  // survived near-clip and culling
    uint32_t trianglesBinned = 0;   // counted once per tile they touch
    uint32_t pixelsTested = 0;      // reached the depth test
    uint32_t pixelsWritten = 0;     // passed it and were shaded

    void Reset() { *this = RasterStats{}; }
};

class Raster3D
{
public:
    /// Point at a framebuffer and start collecting geometry. The buffer is
    /// not cleared: the caller owns the background, as it does in the 2D path.
    void BeginFrame(uint8_t* buffer, int32_t width, int32_t height,
                    Deki::ColorFormat format, const RasterConfig& config);

    /// Transform, clip and bin one mesh. `mvp` takes object space to clip
    /// space; `normalMatrix` takes object-space normals to world space and is
    /// only read by the lit shading models.
    void DrawMesh(const Mesh3D& mesh, const Material3D* materials, uint16_t materialCount,
                  const Deki::Mat4& mvp, const Deki::Mat4& normalMatrix);

    /// Fill every tile. Nothing reaches the framebuffer before this.
    void EndFrame();

    const RasterStats& Stats() const { return m_Stats; }
    const RasterConfig& Config() const { return m_Config; }

private:
    // A vertex after projection, in screen pixels, carrying the attributes the
    // span loop interpolates. uOverW / vOverW / invW are what perspective
    // correction needs; they are linear in screen space, u and v are not.
    struct ScreenVertex
    {
        float x, y;
        float invW;
        float z;  // normalised device z in [-1, 1]
        float uOverW, vOverW;
        uint32_t color;
        float light;
    };

    struct RasterTri
    {
        ScreenVertex v[3];
        // An index into m_Materials, not a pointer at the caller's material.
        // Filling is deferred to EndFrame, by which time anything the caller
        // built on its stack is long gone.
        uint16_t materialIndex;
    };

    // Clip space, before the divide. Sutherland-Hodgman against the near
    // plane works here because that plane is z + w = 0.
    struct ClipVertex
    {
        float x, y, z, w;
        float u, v;
        uint32_t color;
        float light;
    };

    void EmitTriangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c,
                      uint16_t materialIndex);
    void ProjectAndBin(const ClipVertex* poly, int count, uint16_t materialIndex);
    void Bin(const RasterTri& tri);
    void FillTile(int tileX, int tileY);
    void FillTriangleInTile(const RasterTri& tri, int minX, int minY, int maxX, int maxY,
                            uint16_t* tileDepth, int tileOriginX, int tileOriginY);

    uint8_t* m_Buffer = nullptr;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    Deki::ColorFormat m_Format = Deki::ColorFormat::RGB565;
    RasterConfig m_Config;
    Deki::Vector3 m_LightDir;

    int m_TilesX = 0;
    int m_TilesY = 0;

    // Materials copied out of each DrawMesh call, so a caller may pass one
    // that lives only for the duration of that call.
    std::vector<Material3D> m_Materials;
    std::vector<RasterTri> m_Tris;
    std::vector<std::vector<uint32_t>> m_Bins;  // one list of triangle indices per tile
    std::vector<uint16_t> m_TileDepth;

    RasterStats m_Stats;
};

}  // namespace Deki3D
