#pragma once

#include "Deki3DAPI.h"

#include <deki/Component.h>
#include <deki/reflection/Property.h>

namespace Deki3D
{

/// Gives the scene's camera a perspective projection for the 3D pass. Put it
/// on the same object as the DekiRendering::CameraComponent: that component owns the 2D view
/// and the framebuffer, and this one only supplies the three numbers a
/// perspective projection needs. Without one in the scene, meshes are not
/// drawn at all rather than drawn wrongly.
DEKI_CATEGORY("3D")
DEKI_DESCRIPTION("Perspective projection for the 3D pass. Belongs on the camera object.")
DEKI_FORMER_NAME("Camera3DComponent")
class DEKI_3D_API Camera3DComponent : public Deki::Component
{
public:
    DEKI_TOOLTIP("Vertical field of view in degrees. 60 is a common default; larger values look wider and more distorted at the edges.")
    DEKI_RANGE(10, 150)
    DEKI_EXPORT
    float fieldOfView = 60.0f;

    DEKI_TOOLTIP("Nothing closer than this is drawn. Raising it costs nothing and buys depth precision, so keep it as large as the scene allows.")
    DEKI_EXPORT
    float nearPlane = 0.1f;

    DEKI_TOOLTIP("Nothing further than this is drawn.")
    DEKI_EXPORT
    float farPlane = 100.0f;

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
