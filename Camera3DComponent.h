#pragma once

#include "Deki3DAPI.h"

#include <deki/Component.h>
#include <deki/reflection/Property.h>

/// Gives the scene's camera a perspective projection for the 3D pass. Put it
/// on the same object as the CameraComponent: that component owns the 2D view
/// and the framebuffer, and this one only supplies the three numbers a
/// perspective projection needs. Without one in the scene, meshes are not
/// drawn at all rather than drawn wrongly.
DEKI_CATEGORY("3D")
DEKI_DESCRIPTION("Perspective projection for the 3D pass. Belongs on the camera object.")
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
};
