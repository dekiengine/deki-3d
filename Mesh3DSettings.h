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

/// How the 3D pass draws: tile size, the retro switches, the light and the fill
/// threads. The projection belongs to the scene's camera
/// (DekiRendering::CameraComponent with Projection = Perspective). Put one
/// anywhere in the scene, usually on the camera. Without one the pass uses the
/// defaults below.
DEKI_CATEGORY("3D")
DEKI_DESCRIPTION("How the 3D pass draws: tiles, texture and vertex switches, light and threads.")
class DEKI_3D_API Mesh3DSettings : public Deki::Component
{
public:
    DEKI_TOOLTIP("What draws the meshes. Auto uses the GPU where the build has one (the deki-3d-gl package on a "
                 "platform with OpenGL) and the software rasteriser everywhere else. Software is the exact picture a "
                 "board without a GPU shows, so choose it to preview one in the editor; GPU is far faster where "
                 "there is one.")
    DEKI_EXPORT
    Mesh3DRenderer renderer = Mesh3DRenderer::Auto;

    DEKI_TOOLTIP("Tile edge in pixels. Software only. The depth buffer is one tile, so 32 costs 2 KB and suits a microcontroller, "
                 "while a desktop can afford 128 or more.")
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

    DEKI_TOOLTIP("Direction the scene light comes from, swept around the vertical axis. 0 puts it behind the camera, "
                 "90 to the right.")
    DEKI_RANGE(-180, 180)
    DEKI_EXPORT
    float lightYaw = -25.0f;

    DEKI_TOOLTIP("How high the scene light sits. 90 is directly overhead, 0 level with the horizon.")
    DEKI_RANGE(-90, 90)
    DEKI_EXPORT
    float lightPitch = 55.0f;

    DEKI_TOOLTIP("How lit the faces turned away from the light still are. 0 leaves them black, which is dramatic; "
                 "higher flattens the shading out.")
    DEKI_RANGE(0, 1)
    DEKI_EXPORT
    float ambient = 0.25f;

    DEKI_TOOLTIP("Software only: how many threads fill the screen on a device or simulator build. Tiles are "
                 "independent, so this is the one part that scales with cores: 1 starts no threads at all, 2 suits a "
                 "dual-core board, and above the core count it stops helping. The editor always previews with one thread, since it loads "
                 "packages as unloadable DLLs; the picture is identical either way.")
    DEKI_RANGE(1, 16)
    DEKI_EXPORT
    int32_t fillThreads = 1;
};

}  // namespace Deki3D
