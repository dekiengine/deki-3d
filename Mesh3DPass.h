#pragma once

#include "Deki3DAPI.h"
#include "Raster3D.h"

#include "deki-rendering/RenderPass.h"

// The camera the renderer draws through.
namespace DekiRendering
{
class CameraComponent;
}
namespace Deki3D
{

/// Draws every MeshComponent the renderer hands it, into the same framebuffer
/// the 2D content uses.
///
/// A pass, not a renderer: the engine holds exactly one renderer, chosen in
/// project settings, so a separate 3D renderer would rule out 2D. As a pass, a
/// mesh is one more item in the renderer's sort. It draws at its sorting
/// position, 2D interface composites on top, and the camera and clipping keep
/// working.
///
/// Nothing reaches the framebuffer until the tiles are filled, which lets the
/// depth buffer be one tile instead of one screen. So the pass batches
/// consecutive mesh objects and flushes them in PreExecute just before the
/// next 2D object draws. Consecutive meshes share one depth buffer and resolve
/// against each other; a 2D object between two meshes splits them into two
/// batches, as its sorting order asks.
class DEKI_3D_API Mesh3DPass : public DekiRendering::RenderPass
{
public:
    static constexpr const char* kRegistryName = "mesh3d";

    uint32_t HookMask() const override
    {
        return DekiRendering::RenderPassHooks::BeginFrame | DekiRendering::RenderPassHooks::PreExecute |
               DekiRendering::RenderPassHooks::Execute | DekiRendering::RenderPassHooks::EndFrame;
    }

    Mesh3DPass() = default;
    ~Mesh3DPass() override;
    Mesh3DPass(const Mesh3DPass&) = delete;
    Mesh3DPass& operator=(const Mesh3DPass&) = delete;

    void BeginFrame(DekiRendering::RenderContext& ctx) override;
    void PreExecute(Deki::Object* obj, DekiRendering::RenderContext& ctx) override;
    void Execute(Deki::Object* obj, DekiRendering::RenderContext& ctx) override;
    void EndFrame(DekiRendering::RenderContext& ctx) override;

    /// It writes into the buffer only when a batch of meshes ends, and then
    /// says so itself (QuadBlit::NoteWritten), and only if its backend wrote
    /// there: the GPU's, drawing into the display's frame, does not.
    bool WritesBuffer() const override { return false; }

private:
    /// Fills the tiles for everything binned so far and starts a fresh batch.
    void Flush();
    /// Ends the batch, telling a GPU drawing the 2D what it wrote.
    void EndBatch();
    /// Works out the projection, once per frame, when the first object arrives.
    void Start(const Deki::Object* sceneObject);
    /// The GPU backend, made on first use; null where there is none.
    Mesh3DBackend* Gpu();

    Raster3D m_Raster;
    // What draws this frame: m_Raster, or m_Gpu.
    Mesh3DBackend* m_Backend = &m_Raster;
    // Made by CreateGpuBackend, destroyed only by DestroyGpuBackend. Always
    // null where the platform has no OpenGL.
    Mesh3DBackend* m_Gpu = nullptr;
    bool m_GpuTried = false;
    RasterConfig m_Config;
    // Rebuilt per object from the mesh's materials and the component's
    // overrides. A member, so its storage is reused across objects and frames.
    std::vector<Material3D> m_Materials;
    Deki::Mat4 m_ViewProjection;

    // The frame's target, kept so a flush can start the next batch on it.
    uint8_t* m_Buffer = nullptr;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    Deki::ColorFormat m_Format = Deki::ColorFormat::RGB565;

    DekiRendering::CameraComponent* m_Camera = nullptr;

    bool m_Started = false;  // Start() has run for this frame
    bool m_Active = false;   // false when the scene has no 3D camera
    bool m_Pending = false;  // geometry is binned and not yet filled
};

}  // namespace Deki3D
