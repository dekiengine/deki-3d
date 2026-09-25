#include "Mesh3DPass.h"

#include "Mesh3DSettings.h"
#include <deki/ScreenScale.h>
#include "Math3D.h"
#include "MeshComponent.h"

#include "deki-rendering/CameraComponent.h"
#include "deki-rendering/DekiRenderer.h"  // DekiRendering::RenderContext
#include "deki-rendering/DekiRenderPassRegistry.h"

#include <deki/Object.h>
#include <deki/Scene.h>

#include <cmath>

namespace Deki3D
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

/// Two 0xAABBGGRR colours, channel by channel. White leaves the other alone.
uint32_t MultiplyTint(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const uint32_t channel = (((a >> shift) & 0xFF) * ((b >> shift) & 0xFF) + 127) / 255;
        out |= (channel & 0xFF) << shift;
    }
    return out;
}

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

/// Depth-first search of a subtree for the first component that passes `pick`.
///
/// Scene::GetObjects() hands back the roots only, and a camera is almost always
/// a child of one: the scaffold puts every object under a single root.
/// Searching the roots alone found nothing at all.
template <typename T, typename Pick>
const T* FindInSubtree(const Deki::Object* object, Pick pick)
{
    if (!object)
        return nullptr;
    if (const T* found = object->GetComponent<T>(); found && pick(*found))
        return found;
    for (const Deki::Object* child : object->GetChildren())
    {
        if (const T* found = FindInSubtree<T>(child, pick))
            return found;
    }
    return nullptr;
}

template <typename T, typename Pick>
const T* FindInScene(const Deki::Object* anyObject, Pick pick)
{
    const Deki::Scene* scene = anyObject ? anyObject->GetOwnerScene() : nullptr;
    if (!scene)
        return nullptr;
    for (const Deki::Object* root : scene->GetObjects())
    {
        if (const T* found = FindInSubtree<T>(root, pick))
            return found;
    }
    return nullptr;
}

bool IsPerspective(const DekiRendering::CameraComponent& c)
{
    return c.projection == Deki::ProjectionMode::Perspective;
}

}  // namespace

void Mesh3DPass::BeginFrame(DekiRendering::RenderContext& ctx)
{
    m_Active = false;
    m_Started = false;
    m_Pending = false;

    if (!ctx.camera || !ctx.buffer || ctx.width <= 0 || ctx.height <= 0)
        return;

    m_Camera = ctx.camera;
    m_Buffer = ctx.buffer;
    m_Width = ctx.width;
    m_Height = ctx.height;
    m_Format = ctx.format;
}

/// Set up the projection the first time an object comes past.
///
/// Deferred to here rather than done in BeginFrame because finding the
/// scene's 3D camera needs an object that belongs to the scene, and the
/// camera the renderer hands us may not: in the editor it is a standalone
/// object the viewport owns.
void Mesh3DPass::Start(const Deki::Object* sceneObject)
{
    m_Started = true;
    m_Active = false;

    Deki::Object* cameraObject = m_Camera ? m_Camera->GetOwner() : nullptr;
    if (!cameraObject)
        return;

    // A perspective camera draws the meshes through its own view. The editor's
    // viewport renders through a camera of its own, orthographic, which is not
    // in the scene: it borrows the scene's perspective camera's lens instead.
    // A scene with no perspective camera is not a 3D scene and gets nothing
    // drawn rather than a projection invented for it.
    const bool throughSceneCamera = IsPerspective(*m_Camera);
    const DekiRendering::CameraComponent* lens =
        throughSceneCamera ? m_Camera
                           : FindInScene<DekiRendering::CameraComponent>(sceneObject, IsPerspective);
    if (!lens)
        return;

    // Through the scene camera the field of view follows the project's Screen
    // Fit, so the design shape's view survives on screens of other shapes.
    const float fovDegrees = throughSceneCamera
        ? Deki::ResolveVerticalFieldOfView(lens->fieldOfView, m_Width, m_Height)
        : lens->fieldOfView;
    const float fovY = fovDegrees * kPi / 180.0f;

#ifdef DEKI_TRANSFORM_3D
    float camZ = cameraObject->GetWorldZ();
#else
    float camZ = 0.0f;
#endif

    if (!throughSceneCamera)
    {
        // Previewing through the editor's own camera. Pull back to the
        // distance at which the plane through z = 0 covers exactly what the
        // editor is showing in 2D, so zooming the viewport scales the 3D with
        // it rather than leaving it a fixed size.
        const float visibleHeight = m_Camera->GetVisibleHeight(m_Width, m_Height);
        const float halfAngle = std::tan(fovY * 0.5f);
        if (visibleHeight > 0.0f && halfAngle > 0.0f)
            camZ += (visibleHeight * 0.5f) / halfAngle;
    }

    const Deki::Vector3 eye(cameraObject->GetWorldX(), cameraObject->GetWorldY(), camZ);

    // The editor's camera is a 2D pan and zoom with no meaningful orientation,
    // so the preview looks straight down -Z rather than borrowing its rotation.
    const Deki::Mat4 basis =
        throughSceneCamera ? CameraRotation(cameraObject) : Deki::Mat4::Identity();
    const Deki::Vector3 forward = TransformDirection(basis, Deki::Vector3(0.0f, 0.0f, -1.0f));
    const Deki::Vector3 up = TransformDirection(basis, Deki::Vector3(0.0f, 1.0f, 0.0f));

    const float aspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);
    const Deki::Mat4 projection = Perspective(fovY, aspect, lens->nearPlane, lens->farPlane);
    const Deki::Mat4 view = LookAt(eye, eye + forward, up);
    m_ViewProjection = Mul(projection, view);

    // How to draw: the scene's Mesh3DSettings, or its values as declared when
    // the scene has none.
    static const Mesh3DSettings kDeclared{};
    const Mesh3DSettings* settings =
        FindInScene<Mesh3DSettings>(sceneObject, [](const Mesh3DSettings&) { return true; });
    const Mesh3DSettings& cam3d = settings ? *settings : kDeclared;

    m_Config = RasterConfig{};
    m_Config.tileSize = cam3d.tileSize;
    m_Config.perspectiveCorrect = cam3d.perspectiveCorrect;
    m_Config.vertexSnap = cam3d.vertexSnap;

    // Yaw and pitch rather than a raw vector: an author wants to swing a light
    // around, not to normalise one by hand. Yaw 0 puts it behind the viewer.
    const float yaw = cam3d.lightYaw * kPi / 180.0f;
    const float pitch = cam3d.lightPitch * kPi / 180.0f;
    m_Config.lightDirection = Deki::Vector3(std::sin(yaw) * std::cos(pitch),
                                            -std::sin(pitch),
                                            -std::cos(yaw) * std::cos(pitch));
    m_Config.ambient = cam3d.ambient;
#ifdef DEKI_EDITOR
    // One thread inside the editor, whatever the scene asks for.
    //
    // The editor loads packages as DLLs and unloads them again for hot
    // reload. Joining a thread while a DLL is being unloaded deadlocks on the
    // Windows loader lock, and the process cannot then be killed at all, not
    // even forcibly. That is a far worse failure than losing some preview
    // speed, and it is not hypothetical: it wedged two editor processes here
    // before this guard existed.
    //
    // A device or simulator build links its packages instead of loading them,
    // so nothing is ever unloaded and the workers are safe. The output is
    // identical either way, which the rasteriser's own tests assert, so the
    // preview differs from the device in speed alone.
    m_Config.threadCount = 1;
#else
    m_Config.threadCount = cam3d.fillThreads;
#endif

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

void Mesh3DPass::PreExecute(Deki::Object* obj, DekiRendering::RenderContext& ctx)
{
    (void)ctx;
    if (!m_Started && obj)
        Start(obj);
    // A 2D object is about to draw. Anything binned so far sorts before it,
    // so it has to reach the framebuffer first.
    if (!m_Active || !m_Pending || !obj)
        return;
    if (obj->GetComponent<MeshComponent>())
        return;  // still in a run of meshes: keep batching
    Flush();
}

void Mesh3DPass::Execute(Deki::Object* obj, DekiRendering::RenderContext& ctx)
{
    (void)ctx;
    if (!m_Started && obj)
        Start(obj);
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

    // Materials come from the mesh, which carries one per `usemtl` group with
    // its own texture. The component's own settings are applied on top: its
    // tint multiplies each material's, its shading model replaces theirs
    // (shading is a property of how you want the object drawn, not of the
    // model file), and its double-sided switch can only add.
    const Deki3D::MeshAsset* asset = mesh->Asset();
    const uint32_t componentTint = mesh->tintColor.ToRGBA8888();

    m_Materials.clear();
    if (asset && asset->MaterialCount() > 0)
        m_Materials.assign(asset->Materials(), asset->Materials() + asset->MaterialCount());
    else
        m_Materials.emplace_back();

    for (Material3D& material : m_Materials)
    {
        material.tint = MultiplyTint(material.tint, componentTint);
        material.shading = mesh->shading;
        material.doubleSided = material.doubleSided || mesh->doubleSided;
    }

    // Rotation only for normals: uniform scale leaves them pointing the right
    // way, and the shading models here are not worth an inverse transpose.
#ifdef DEKI_TRANSFORM_3D
    const Deki::Mat4 normalMatrix = Mul(RotateZ(obj->GetWorldRotation()),
                                        Mul(RotateY(obj->GetWorldRotationY()),
                                            RotateX(obj->GetWorldRotationX())));
#else
    const Deki::Mat4 normalMatrix = RotateZ(obj->GetWorldRotation());
#endif

    m_Raster.DrawMesh(*geometry, m_Materials.data(), static_cast<uint16_t>(m_Materials.size()),
                      Mul(m_ViewProjection, model), normalMatrix);
    m_Pending = true;
}

void Mesh3DPass::EndFrame(DekiRendering::RenderContext& ctx)
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
        DekiRendering::RenderPassInfo info;
        info.factory = []() -> DekiRendering::RenderPass* { return new Deki3D::Mesh3DPass(); };
        info.autoAttach = true;
        DekiRendering::DekiRenderPassRegistry::Register(Deki3D::Mesh3DPass::RegistryName, info);
    }
    ~Mesh3DPassRegistrar()
    {
        DekiRendering::DekiRenderPassRegistry::Unregister(Deki3D::Mesh3DPass::RegistryName);
    }
};
static Mesh3DPassRegistrar s_mesh3dPassRegistrar;
}  // namespace
