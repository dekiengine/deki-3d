#pragma once

#include "Deki3DAPI.h"

#include <deki/Component.h>
#include <deki/reflection/Property.h>

#include <cstdint>

namespace Deki3D
{

/// What draws the meshes (Mesh3DBackend.h).
enum class Mesh3DRenderer : uint8_t
{
    Auto = 0,      // the GPU where the build has one, software elsewhere
    Software = 1,  // always the software rasteriser
    Gpu = 2,       // the GPU; software, with a warning, where there is none
};

/// How the 3D pass draws: the renderer, tile size, the retro switches and the
/// fill threads. The projection belongs to the scene's camera
/// (DekiRendering::CameraComponent with Projection = Perspective), and the
/// light to the scene's Light components. Put one anywhere in the scene,
/// usually on the camera. Without one the pass uses the defaults below.
DEKI_CATEGORY("3D")
DEKI_DESCRIPTION("How the 3D pass draws: renderer, tiles, texture and vertex switches, threads.")
class DEKI_3D_API Mesh3DSettings : public Deki::Component
{
public:
    DEKI_TOOLTIP("What draws the meshes. Auto uses the GPU where the platform has OpenGL and the software "
                 "rasteriser everywhere else. Software is the exact picture a "
                 "board without a GPU shows, so choose it to preview one in the editor; GPU is far faster where "
                 "there is one.")
    DEKI_EXPORT
    Mesh3DRenderer renderer = Mesh3DRenderer::Auto;

    DEKI_TOOLTIP("GPU only: build the frame on the GPU where the display presents through it, drawing the meshes "
                 "straight into it between layers of 2D instead of reading them back. Much faster on a handheld. "
                 "Turn it off if something later in the frame reads the whole picture back from the frame buffer "
                 "(a full-screen effect pass); with it on, that buffer holds only the 2D drawn after the meshes.")
    DEKI_EXPORT
    bool composeOnGpu = true;

    DEKI_TOOLTIP("Tile edge in pixels. Software only. The depth buffer is one tile, so 32 costs 2 KB and suits a "
                 "microcontroller, while a desktop can afford 128 or more.")
    DEKI_RANGE(8, 512)
    DEKI_EXPORT
    int32_t tileSize = 32;

    DEKI_TOOLTIP("Correct texture perspective. Turning it off gives the warping of PlayStation-era hardware, and saves "
                 "about a tenth of the fill cost.")
    DEKI_EXPORT
    bool perspectiveCorrect = true;

    DEKI_TOOLTIP("Round projected vertices to whole pixels, for the jitter of fixed-point era hardware.")
    DEKI_EXPORT
    bool vertexSnap = false;

    DEKI_TOOLTIP("Software only: how many threads draw on a device or simulator build: they fill the screen's tiles, "
                 "and share out the geometry of each part of a mesh of a few hundred triangles or more. 1 starts no "
                 "threads at all, 2 suits a dual-core board, and above the core count it stops helping. The editor "
                 "always previews with one thread, since it loads packages as unloadable DLLs; the picture is "
                 "identical either way.")
    DEKI_RANGE(1, 16)
    DEKI_EXPORT
    int32_t fillThreads = 1;
};

}  // namespace Deki3D
