#include "Mesh3DPass.h"

#include "Camera3DComponent.h"
#include "Math3D.h"
#include "MeshComponent.h"

#include "deki-rendering/CameraComponent.h"
#include "deki-rendering/DekiRenderer.h"  // RenderContext
#include "deki-rendering/DekiRenderPassRegistry.h"

#include <deki/Object.h>

#include <cmath>

namespace Deki3D
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

/// The camera's own world rotation, as a basis. The engine stores angles, so
/// the view matrix is built from a position and a direction rather than by
/// inverting a matrix.
Deki::Mat4 CameraRotation(const Deki::Object* obj)
{
#ifdef DEKI_TRANSFORM_3D
    return Mul(RotateZ(obj->GetWorldRotation()),
               Mul(RotateY(obj->GetWorldRotationY()), RotateX(obj->GetWorldRotationX())));
#else
    return RotateZ(obj->GetWorldRotation());
#endif
}

}  // namespace

void Mesh3DPass::BeginFrame(RenderContext& ctx)
{
    m_Active = false;
    if (!ctx.camera || !ctx.buffer || ctx.width <= 0 || ctx.height <= 0)
        return;

    // RGB565 is the only format the span loop writes. Every Deki display path
    // uses it; the others would need a second inner loop and no caller wants
    // one yet.
    if (ctx.format != Deki::ColorFormat::RGB565)
        return;

    Deki::Object* cameraObject = ctx.camera->GetOwner();
    if (!cameraObject)
        return;

    // No 3D camera means this scene is not a 3D scene. Draw nothing rather
    // than inventing a projection.
    const Camera3DComponent* cam3d = cameraObject->GetComponent<Camera3DComponent>();
    if (!cam3d)
        return;

#ifdef DEKI_TRANSFORM_3D
    const float camZ = cameraObject->GetWorldZ();
#else
    const float camZ = 0.0f;
#endif
    const Deki::Vector3 eye(cameraObject->GetWorldX(), cameraObject->GetWorldY(), camZ);

    const Deki::Mat4 basis = CameraRotation(cameraObject);
    const Deki::Vector3 forward = TransformDirection(basis, Deki::Vector3(0.0f, 0.0f, -1.0f));
    const Deki::Vector3 up = TransformDirection(basis, Deki::Vector3(0.0f, 1.0f, 0.0f));

    const float aspect = static_cast<float>(ctx.width) / static_cast<float>(ctx.height);
    const float fovY = cam3d->fieldOfView * kPi / 180.0f;
    const Deki::Mat4 projection = Perspective(fovY, aspect, cam3d->nearPlane, cam3d->farPlane);
    const Deki::Mat4 view = LookAt(eye, eye + forward, up);
    m_ViewProjection = Mul(projection, view);

    m_Config = RasterConfig{};
    m_Config.tileSize = cam3d->tileSize;
    m_Config.perspectiveCorrect = cam3d->perspectiveCorrect;
    m_Config.vertexSnap = cam3d->vertexSnap;

    m_Buffer = ctx.buffer;
    m_Width = ctx.width;
    m_Height = ctx.height;
    m_Format = ctx.format;

    m_Raster.BeginFrame(m_Buffer, m_Width, m_Height, m_Format, m_Config);
    m_Active = true;
    m_Pending = false;
}

void Mesh3DPass::Flush()
{
    if (!m_Pending)
        return;
    m_Raster.EndFrame();
    m_Raster.BeginFrame(m_Buffer, m_Width, m_Height, m_Format, m_Config);
    m_Pending = false;
}

void Mesh3DPass::PreExecute(Deki::Object* obj, RenderContext& ctx)
{
    (void)ctx;
    // A 2D object is about to draw. Anything binned so far sorts before it,
    // so it has to reach the framebuffer first.
    if (!m_Active || !m_Pending || !obj)
        return;
    if (obj->GetComponent<MeshComponent>())
        return;  // still in a run of meshes: keep batching
    Flush();
}

void Mesh3DPass::Execute(Deki::Object* obj, RenderContext& ctx)
{
    (void)ctx;
    if (!m_Active || !obj)
        return;

    MeshComponent* mesh = obj->GetComponent<MeshComponent>();
    if (!mesh)
        return;

    const Mesh3D* geometry = mesh->Resolve();
    if (!geometry)
        return;

#ifdef DEKI_TRANSFORM_3D
    const Deki::Mat4 model = Compose(obj->GetWorldX(), obj->GetWorldY(), obj->GetWorldZ(),
                                     obj->GetWorldRotationX(), obj->GetWorldRotationY(),
                                     obj->GetWorldRotation(),
                                     obj->GetWorldScaleX(), obj->GetWorldScaleY(),
                                     obj->GetWorldScaleZ());
#else
    const Deki::Mat4 model = Compose(obj->GetWorldX(), obj->GetWorldY(), 0.0f,
                                     0.0f, 0.0f, obj->GetWorldRotation(),
                                     obj->GetWorldScaleX(), obj->GetWorldScaleY(), 1.0f);
#endif

    Material3D material;
    material.texture = nullptr;
    material.tint = mesh->tintColor.ToRGBA8888();
    material.shading = mesh->shading;
    material.doubleSided = mesh->doubleSided;

    // Rotation only for normals: uniform scale leaves them pointing the right
    // way, and the shading models here are not worth an inverse transpose.
#ifdef DEKI_TRANSFORM_3D
    const Deki::Mat4 normalMatrix = Mul(RotateZ(obj->GetWorldRotation()),
                                        Mul(RotateY(obj->GetWorldRotationY()),
                                            RotateX(obj->GetWorldRotationX())));
#else
    const Deki::Mat4 normalMatrix = RotateZ(obj->GetWorldRotation());
#endif

    m_Raster.DrawMesh(*geometry, &material, 1, Mul(m_ViewProjection, model), normalMatrix);
    m_Pending = true;
}

void Mesh3DPass::EndFrame(RenderContext& ctx)
{
    (void)ctx;
    if (!m_Active)
        return;
    m_Raster.EndFrame();  // whatever the last run of meshes left binned
    m_Active = false;
    m_Pending = false;
}

}  // namespace Deki3D

// Self-registration with autoAttach, so a project that installs deki-3d gets
// the pass without naming it in its render pipeline. Unregistering on unload
// keeps the factory, whose target lives in this package's code, from
// outliving the DLL.
namespace
{
struct Mesh3DPassRegistrar
{
    Mesh3DPassRegistrar()
    {
        RenderPassInfo info;
        info.factory = []() -> RenderPass* { return new Deki3D::Mesh3DPass(); };
        info.autoAttach = true;
        DekiRenderPassRegistry::Register(Deki3D::Mesh3DPass::RegistryName, info);
    }
    ~Mesh3DPassRegistrar()
    {
        DekiRenderPassRegistry::Unregister(Deki3D::Mesh3DPass::RegistryName);
    }
};
static Mesh3DPassRegistrar s_mesh3dPassRegistrar;
}  // namespace
