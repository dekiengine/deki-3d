#pragma once

#include "Deki3DAPI.h"
#include "Raster3D.h"

#include "deki-rendering/RenderPass.h"

namespace Deki3D
{

/// Draws every MeshComponent the renderer hands it, into the same framebuffer
/// the 2D content uses.
///
/// A pass rather than a renderer, deliberately. The engine's render system
/// holds exactly one renderer, chosen by name from project settings, so a
/// separate 3D renderer would mean giving up 2D entirely. As a pass, a mesh
/// is simply another item in the renderer's existing sort: 3D draws at its
/// sorting position and a 2D interface composites on top of it for free, and
/// the camera, clipping and dirty-rectangle tracking keep working untouched.
///
/// Nothing reaches the framebuffer until the tiles are filled, which is what
/// lets the depth buffer be one tile instead of one screen. That deferral is
/// also why the pass watches the sort: meshes accumulate while consecutive
/// mesh objects arrive, and the batch is flushed in PreExecute just before
/// the first 2D object that sorts after them draws. So a mesh group lands at
/// its own place in the sort, an interface above it composites on top, and a
/// backdrop below it stays behind. Consecutive meshes share one depth buffer
/// and resolve against each other; a 2D object between two meshes splits them
/// into two batches, as its sorting order asks for.
class DEKI_3D_API Mesh3DPass : public RenderPass
{
public:
    static constexpr const char* RegistryName = "mesh3d";

    uint32_t HookMask() const override
    {
        return RenderPassHooks::BeginFrame | RenderPassHooks::PreExecute |
               RenderPassHooks::Execute | RenderPassHooks::EndFrame;
    }

    void BeginFrame(RenderContext& ctx) override;
    void PreExecute(Deki::Object* obj, RenderContext& ctx) override;
    void Execute(Deki::Object* obj, RenderContext& ctx) override;
    void EndFrame(RenderContext& ctx) override;

private:
    /// Fill the tiles for everything binned so far and start a fresh batch.
    void Flush();

    Raster3D m_Raster;
    RasterConfig m_Config;
    Deki::Mat4 m_ViewProjection;

    // The frame's target, kept so a flush can start the next batch on it.
    uint8_t* m_Buffer = nullptr;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    Deki::ColorFormat m_Format = Deki::ColorFormat::RGB565;

    bool m_Active = false;   // false when the scene has no 3D camera
    bool m_Pending = false;  // geometry is binned and not yet filled
};

}  // namespace Deki3D
