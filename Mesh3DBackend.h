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

/// One light as the meshes see it, in world space: the scene's Light
/// components (deki-rendering's Light.h), as Mesh3DPass gathers them.
struct Light3D
{
    enum class Kind : uint8_t
    {
        Directional = 0,
        Point = 1,
        Spot = 2,
    };
    Kind kind = Kind::Directional;
    Deki::Vector3 color = Deki::Vector3(1.0f, 1.0f, 1.0f);  // times its intensity: 1 lights a colour as it is
    Deki::Vector3 position;                                 // point and spot
    Deki::Vector3 direction = Deki::Vector3(0.0f, -1.0f, 0.0f);  // the way it travels, unit: directional and spot
    float range = 1.0f;                                          // point and spot: where it fades to nothing
    float cosOuter = 0.0f;  // spot: the cosines of its cone's edge, and of where it starts to fade
    float cosInner = 0.0f;
    int shadow = -1;  // the shadow map it casts into (RasterConfig::shadows), or -1 for none
};

/// A shadow map: a light's view of the shadow casters, which the GPU draws
/// before the frame's meshes (Mesh3DBackend::DrawShadowCaster).
struct ShadowMap
{
    Deki::Mat4 viewProjection;  // world to the light's clip space
    int resolution = 1024;      // pixels on each side
    float bias = 0.002f;        // depth pushed back, against a surface shadowing itself
};

/// The light reaching a surface at `position` facing `normal` (unit), world
/// space: the ambient, plus each light by how near it is and how much the
/// surface faces it, capped at twice a colour. The software rasteriser lights
/// with it; the GPU's mesh shader does the same sum (editor/MeshShaderUse.cpp).
DEKI_3D_API Deki::Vector3 LightAt(const Deki::Vector3& ambient, const Light3D* lights, int count,
                                  const Deki::Vector3& position, const Deki::Vector3& normal);

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

    /// The light of the lit shading models: the ambient, and up to kMaxLights
    /// lights. The default is a grey ambient and a sun from above, in front,
    /// to the left; Mesh3DPass sets the scene's.
    static constexpr int kMaxLights = 8;
    Deki::Vector3 ambient = Deki::Vector3(0.25f, 0.25f, 0.25f);
    Light3D lights[kMaxLights];
    int lightCount = 1;

    /// Shadow maps, drawn only where the backend can (DrawsShadows).
    static constexpr int kMaxShadows = 2;
    ShadowMap shadows[kMaxShadows];
    int shadowCount = 0;

    RasterConfig()
    {
        lights[0].color = Deki::Vector3(0.75f, 0.75f, 0.75f);
        lights[0].direction = Deki::Vector3(-0.4f, -0.8f, -0.45f);
    }
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
    /// `normalMatrix` takes object-space normals to world space and `model`
    /// positions to world space, read only by the lit shading models (a light
    /// that is near or far needs the world position). The materials need only
    /// live for the call.
    virtual void DrawMesh(const Mesh3D& mesh, const Material3D* materials, uint16_t materialCount,
                          const Deki::Mat4& mvp, const Deki::Mat4& normalMatrix,
                          const Deki::Mat4& model = Deki::Mat4::Identity()) = 0;

    /// Finishes the batch. Nothing need reach the framebuffer before this.
    virtual void EndFrame() = 0;

    /// Whether this backend draws shadows: the GPU's does, the software
    /// rasteriser does not.
    virtual bool DrawsShadows() const { return false; }

    /// Draws `mesh` into shadow map `slot` of the batch's config, depth only:
    /// a shadow caster as its light sees it. Called after BeginFrame and before
    /// its first DrawMesh, for each caster and each map; the maps then hold
    /// for every batch of the frame. Each material's shader moves the vertices
    /// and its texture cuts holes, as when the mesh is drawn.
    virtual void DrawShadowCaster(int slot, const Mesh3D& mesh, const Material3D* materials, uint16_t materialCount,
                                  const Deki::Mat4& model)
    {
        (void)slot;
        (void)mesh;
        (void)materials;
        (void)materialCount;
        (void)model;
    }

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
