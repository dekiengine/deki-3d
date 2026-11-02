#pragma once

// What draws Mesh3DPass's meshes: the software rasteriser (Raster3D), or the
// GPU backend (GLMesh3DBackend). Both take the same frame - BeginFrame,
// DrawMesh per mesh, EndFrame - into the same CPU framebuffer the 2D content
// draws into, so the pass, its sorting against 2D and every scene work the
// same whichever one draws.
//
// The GPU backend exists only where the platform has OpenGL
// (DEKI_CAPABILITY_OPENGL): a board without a GPU builds the software
// rasteriser alone.

#include <deki/Engine.h>  // Deki::ColorFormat
#include <deki/Vector.h>

#include "Deki3DAPI.h"
#include "Mesh3D.h"

#include <cstdint>

namespace Deki3D
{

struct RasterConfig
{
    /// Tile edge in pixels. The per-tile depth buffer is tileSize^2 * 2 bytes.
    /// Software only.
    int tileSize = 32;

    /// Splits each span and does the perspective divide at the split points,
    /// interpolating linearly between. The divide then costs once per
    /// `spanSubdivision` pixels instead of once per pixel, so correct
    /// texturing is nearly as cheap as affine. Off, texturing is affine, the
    /// warping of PlayStation-era hardware, on every backend.
    bool perspectiveCorrect = true;
    int spanSubdivision = 16;

    /// Rounds projected vertices to whole pixels, for the PlayStation look
    /// (its transform was integer).
    bool vertexSnap = false;

    bool backfaceCull = true;
    bool depthTest = true;
    bool depthWrite = true;

    /// How many threads fill tiles. Tiles are independent and each owns its
    /// depth buffer, so the fill scales without coordination. 1 starts no
    /// threads, for a single-core target; 2 suits a dual-core microcontroller.
    /// Transform, clipping and binning stay on the calling thread, because
    /// they append to shared buffers. Software only.
    int threadCount = 1;

    /// A GPU backend may build the frame on the GPU when the display presents
    /// through one (deki-rendering's GLPresenter): the meshes then draw straight into
    /// the frame between layers of 2D, and the buffer is left transparent
    /// after a batch, holding only the 2D drawn since. Off, the backend writes
    /// its pixels into the buffer, which then holds the whole picture, as
    /// anything that reads the frame back afterwards needs.
    bool composeOnGpu = true;

    /// Direction the one directional light travels, for the lit shading
    /// models. The backend normalises it.
    Deki::Vector3 lightDirection = Deki::Vector3(-0.4f, -0.8f, -0.45f);
    float ambient = 0.25f;
};

struct RasterStats
{
    uint32_t meshesCulled = 0;      // skipped whole, their bounds being off-screen
    uint32_t trianglesIn = 0;       // handed to DrawMesh
    uint32_t trianglesClipped = 0;  // survived near-clip and culling
    uint32_t trianglesBinned = 0;   // counted once per tile they touch
    uint32_t pixelsTested = 0;      // reached the depth test
    uint32_t pixelsWritten = 0;     // passed it and were shaded

    void Reset() { *this = RasterStats{}; }
};

class Mesh3DBackend
{
public:
    virtual ~Mesh3DBackend() = default;

    /// Targets a framebuffer and starts a batch. The buffer is not cleared:
    /// the caller owns the background, as in the 2D path, and the meshes draw
    /// over it with a depth buffer of their own.
    virtual void BeginFrame(uint8_t* buffer, int32_t width, int32_t height, Deki::ColorFormat format,
                            const RasterConfig& config) = 0;

    /// Draws one mesh, or queues it. `mvp` takes object space to clip space;
    /// `normalMatrix` takes object-space normals to world space and is read
    /// only by the lit shading models. The materials need only live for the
    /// call.
    virtual void DrawMesh(const Mesh3D& mesh, const Material3D* materials, uint16_t materialCount,
                          const Deki::Mat4& mvp, const Deki::Mat4& normalMatrix) = 0;

    /// Finishes the batch. Nothing need reach the framebuffer before this.
    virtual void EndFrame() = 0;

    /// Whether the batch's meshes reach the frame through the buffer: the
    /// software rasteriser's do, and a GPU backend's that it reads back. A
    /// GPU backend drawing straight into a frame the display builds on the
    /// GPU writes none. Asked after EndFrame; where the 2D is drawn on the
    /// GPU too (QuadBlit::BlitSink), what a batch wrote is composed over that
    /// frame in its place. The default is yes.
    virtual bool WritesBuffer() const { return true; }
};

#ifdef DEKI_CAPABILITY_OPENGL
/// A GPU backend (GLMesh3DBackend.cpp), or null when the GPU cannot be used
/// here (no driver, no context): the pass then draws in software. Only
/// DestroyGpuBackend may delete what this makes. At the end of the program,
/// or when the editor unloads the package, the backends still alive go
/// first, and DestroyGpuBackend on one of them does nothing.
DEKI_3D_API Mesh3DBackend* CreateGpuBackend();
DEKI_3D_API void DestroyGpuBackend(Mesh3DBackend* backend);
/// For the log: "OpenGL 3.3" or "OpenGL ES 3.0".
DEKI_3D_API const char* GpuBackendName();
#endif

/// A fresh, never-zero Mesh3D or Texture3D revision, for whoever builds one
/// over new data. A GPU backend keeps uploads by revision.
DEKI_3D_API uint32_t NextRevision();

}  // namespace Deki3D
