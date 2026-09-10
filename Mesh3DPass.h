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
/// The three hooks map onto the rasteriser's three phases: BeginFrame starts
/// it and builds the view and projection, Execute bins one object's geometry,
/// EndFrame fills the tiles. Nothing reaches the framebuffer until EndFrame,
/// which is what lets the depth buffer be one tile instead of one screen.
class DEKI_3D_API Mesh3DPass : public RenderPass
{
public:
    static constexpr const char* RegistryName = "mesh3d";

    uint32_t HookMask() const override
    {
        return RenderPassHooks::BeginFrame | RenderPassHooks::Execute | RenderPassHooks::EndFrame;
    }

    void BeginFrame(RenderContext& ctx) override;
    void Execute(Deki::Object* obj, RenderContext& ctx) override;
    void EndFrame(RenderContext& ctx) override;

private:
    Raster3D m_Raster;
    Deki::Mat4 m_ViewProjection;
    bool m_Active = false;  // false when the scene has no 3D camera
};

}  // namespace Deki3D
