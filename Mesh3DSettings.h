#pragma once

#include "Deki3DAPI.h"

#include <deki/Component.h>
#include <deki/reflection/Property.h>

namespace Deki3D
{

/// How the 3D pass draws: tile size, the retro switches, the light and the fill
/// threads. The projection itself belongs to the scene's camera
/// (DekiRendering::CameraComponent with Projection = Perspective); this used to
/// be Camera3DComponent and carried the field of view too, which moved there so
/// one camera holds the whole view. Put one anywhere in the scene, usually on
/// the camera. Without one the pass uses the values below as they stand.
DEKI_CATEGORY("3D")
DEKI_DESCRIPTION("How the 3D pass draws: tiles, texture and vertex switches, light and threads.")
DEKI_FORMER_NAME("Deki3D::Camera3DComponent")
DEKI_FORMER_NAME("Camera3DComponent")
class DEKI_3D_API Mesh3DSettings : public Deki::Component
{
public:
    DEKI_TOOLTIP("Tile edge in pixels. The depth buffer is one tile, so 32 costs 2 KB and suits a microcontroller, while a desktop can afford 128 or more.")
    DEKI_RANGE(8, 512)
    DEKI_EXPORT
    int32_t tileSize = 32;

    DEKI_TOOLTIP("Correct texture perspective. Turning it off gives the warping of PlayStation-era hardware, and saves about a tenth of the fill cost.")
    DEKI_EXPORT
    bool perspectiveCorrect = true;

    DEKI_TOOLTIP("Round projected vertices to whole pixels, for the jitter of fixed-point era hardware.")
    DEKI_EXPORT
    bool vertexSnap = false;

    DEKI_TOOLTIP("Direction the scene light comes from, swept around the vertical axis. 0 puts it behind the camera, 90 to the right.")
    DEKI_RANGE(-180, 180)
    DEKI_EXPORT
    float lightYaw = -25.0f;

    DEKI_TOOLTIP("How high the scene light sits. 90 is directly overhead, 0 level with the horizon.")
    DEKI_RANGE(-90, 90)
    DEKI_EXPORT
    float lightPitch = 55.0f;

    DEKI_TOOLTIP("How lit the faces turned away from the light still are. 0 leaves them black, which is dramatic; higher flattens the shading out.")
    DEKI_RANGE(0, 1)
    DEKI_EXPORT
    float ambient = 0.25f;

    DEKI_TOOLTIP("How many threads fill the screen on a device or simulator build. Tiles are independent, so this is the one part that scales with cores: 1 starts no threads at all, 2 suits a dual-core board, and above the core count it stops helping. The editor always previews with one thread, since it loads packages as unloadable DLLs; the picture is identical either way.")
    DEKI_RANGE(1, 16)
    DEKI_EXPORT
    int32_t fillThreads = 1;
};

}  // namespace Deki3D
