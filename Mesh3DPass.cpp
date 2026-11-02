#include "Mesh3DPass.h"
#include "Deki3DInit.h"

#include "Mesh3DSettings.h"
#include "Math3D.h"
#include "MeshComponent.h"

#include "deki-rendering/CameraComponent.h"
#include "deki-rendering/DekiRenderer.h"  // DekiRendering::RenderContext
#include "deki-rendering/DekiRenderPassRegistry.h"

#include <deki/LogSystem.h>
#include <deki/Object.h>
#include <deki/Scene.h>

#include <cmath>

namespace Deki3D
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

/// Multiplies two 0xAABBGGRR colours channel by channel. White leaves the
/// other unchanged.
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

/// The camera's world rotation as a basis. The engine stores angles, so the
/// view matrix is built from a position and a direction, not by inverting a
/// matrix.
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
/// Must search below the roots: Scene::GetObjects() returns only the roots,
/// and the scaffold puts every object, the camera included, under one root.
template <typename T, typename Pick>
const T* FindInSubtree(const Deki::Object* object, Pick pick)
{
    if (!object)
    {
        return nullptr;
    }
    if (const T* found = object->GetComponent<T>(); found && pick(*found))
    {
        return found;
    }
    for (const Deki::Object* child : object->GetChildren())
    {
        if (const T* found = FindInSubtree<T>(child, pick))
        {
            return found;
        }
    }
    return nullptr;
}

template <typename T, typename Pick>
const T* FindInScene(const Deki::Object* anyObject, Pick pick)
{
    const Deki::Scene* scene = anyObject ? anyObject->GetOwnerScene() : nullptr;
    if (!scene)
    {
        return nullptr;
    }
    for (const Deki::Object* root : scene->GetObjects())
    {
        if (const T* found = FindInSubtree<T>(root, pick))
        {
            return found;
        }
    }
    return nullptr;
}

bool IsPerspective(const DekiRendering::CameraComponent& c)
{
    return c.projection == Deki::ProjectionMode::Perspective;
}

}  // namespace

Mesh3DPass::~Mesh3DPass()
{
    // Only while its provider is the one that made it: a provider that went
    // away has destroyed it already.
    if (m_Gpu && m_GpuProvider && GpuBackendGeneration() == m_GpuGeneration)
    {
        m_GpuProvider->destroy(m_Gpu);
    }
}

Mesh3DBackend* Mesh3DPass::Gpu()
{
    const uint32_t generation = GpuBackendGeneration();
    if (generation != m_GpuGeneration)
    {
        // The provider was set or removed since. Whatever the old one made is
        // gone or not ours to touch, so start over with the current one.
        m_Gpu = nullptr;
        m_GpuProvider = nullptr;
        m_GpuUnavailable = false;
        m_GpuGeneration = generation;
    }
    if (!m_Gpu && !m_GpuUnavailable)
    {
        m_GpuProvider = GetGpuBackendProvider();
        m_Gpu = m_GpuProvider ? m_GpuProvider->create() : nullptr;
        m_GpuUnavailable = m_Gpu == nullptr;
        if (m_Gpu)
        {
            DEKI_LOG_INFO("Mesh3DPass: drawing meshes with %s", m_GpuProvider->name);
        }
    }
    return m_Gpu;
}

void Mesh3DPass::BeginFrame(DekiRendering::RenderContext& ctx)
{
    m_Active = false;
    m_Started = false;
    m_Pending = false;

    if (!ctx.camera || !ctx.buffer || ctx.width <= 0 || ctx.height <= 0)
    {
        return;
    }

    m_Camera = ctx.camera;
    m_Buffer = ctx.buffer;
    m_Width = ctx.width;
    m_Height = ctx.height;
    m_Format = ctx.format;
}

/// Sets up the projection when the first object comes past. Not done in
/// BeginFrame: finding the scene's 3D camera needs an object in the scene, and
/// the renderer's camera may not be one (in the editor the viewport owns it).
void Mesh3DPass::Start(const Deki::Object* sceneObject)
{
    m_Started = true;
    m_Active = false;

    Deki::Object* cameraObject = m_Camera ? m_Camera->GetOwner() : nullptr;
    if (!cameraObject)
    {
        return;
    }

    // A perspective camera draws the meshes through its own view. The editor's
    // viewport camera is orthographic and not in the scene, so it borrows the
    // lens of the scene's perspective camera. A scene with no perspective
    // camera draws no meshes.
    const bool throughSceneCamera = IsPerspective(*m_Camera);
    const DekiRendering::CameraComponent* lens =
        throughSceneCamera ? m_Camera : FindInScene<DekiRendering::CameraComponent>(sceneObject, IsPerspective);
    if (!lens)
    {
        return;
    }

    // The field of view is vertical on every screen; a wider one sees more at
    // the sides.
    const float fovY = lens->fieldOfView * kPi / 180.0f;

#ifdef DEKI_TRANSFORM_3D
    float camZ = cameraObject->GetWorldZ();
#else
    float camZ = 0.0f;
#endif

    if (!throughSceneCamera)
    {
        // Previewing through the editor's camera. Pull back until the plane
        // z = 0 covers exactly what the editor shows in 2D, so zooming the
        // viewport scales the 3D with it.
        const float visibleHeight = m_Camera->GetVisibleHeight(m_Width, m_Height);
        const float halfAngle = std::tan(fovY * 0.5f);
        if (visibleHeight > 0.0f && halfAngle > 0.0f)
        {
            camZ += (visibleHeight * 0.5f) / halfAngle;
        }
    }

    const Deki::Vector3 eye(cameraObject->GetWorldX(), cameraObject->GetWorldY(), camZ);

    // The editor's camera is a 2D pan and zoom, so the preview looks straight
    // down -Z instead of using its rotation.
    const Deki::Mat4 basis = throughSceneCamera ? CameraRotation(cameraObject) : Deki::Mat4::Identity();
    const Deki::Vector3 forward = TransformDirection(basis, Deki::Vector3(0.0f, 0.0f, -1.0f));
    const Deki::Vector3 up = TransformDirection(basis, Deki::Vector3(0.0f, 1.0f, 0.0f));

    const float aspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);
    const Deki::Mat4 projection = Perspective(fovY, aspect, lens->nearPlane, lens->farPlane);
    const Deki::Mat4 view = LookAt(eye, eye + forward, up);
    m_ViewProjection = Mul(projection, view);

    // How to draw: the scene's Mesh3DSettings, or its defaults when the scene
    // has none.
    static const Mesh3DSettings kDeclared{};
    const Mesh3DSettings* settings =
        FindInScene<Mesh3DSettings>(sceneObject, [](const Mesh3DSettings&) { return true; });
    const Mesh3DSettings& cam3d = settings ? *settings : kDeclared;

    m_Config = RasterConfig{};
    m_Config.tileSize = cam3d.tileSize;
    m_Config.perspectiveCorrect = cam3d.perspectiveCorrect;
    m_Config.vertexSnap = cam3d.vertexSnap;

    // Yaw and pitch rather than a vector, so an author can swing the light
    // around without normalising by hand. Yaw 0 puts it behind the viewer.
    const float yaw = cam3d.lightYaw * kPi / 180.0f;
    const float pitch = cam3d.lightPitch * kPi / 180.0f;
    m_Config.lightDirection =
        Deki::Vector3(std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
    m_Config.ambient = cam3d.ambient;
    // Only when each frame starts from a clear: a frame kept from the last
    // one must be whole in the buffer, not partly on the GPU.
    m_Config.composeOnGpu = cam3d.composeOnGpu && m_Camera->clearEveryFrame;
#ifdef DEKI_EDITOR
    // Must stay one thread inside the editor, whatever the scene asks for.
    // The editor unloads package DLLs for hot reload, and joining a thread
    // while a DLL unloads deadlocks on the Windows loader lock; the process
    // then cannot be killed, not even forcibly.
    //
    // A device or simulator build links its packages, so nothing is unloaded
    // and the workers are safe. The output is identical either way (the
    // rasteriser's tests assert it); only the speed differs.
    m_Config.threadCount = 1;
#else
    m_Config.threadCount = cam3d.fillThreads;
#endif

    // The GPU unless the scene asks for software or there is none. A scene
    // that insists on the GPU where there is none still draws, in software,
    // and says why once.
    m_Backend = &m_Raster;
    if (cam3d.renderer != Mesh3DRenderer::Software)
    {
        if (Mesh3DBackend* gpu = Gpu())
        {
            m_Backend = gpu;
        }
        else if (cam3d.renderer == Mesh3DRenderer::Gpu)
        {
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                DEKI_LOG_WARNING("Mesh3DPass: the scene asks for the GPU, but this build has no GPU backend (install "
                                 "deki-3d-gl on a platform with OpenGL); drawing in software");
            }
        }
    }

    m_Backend->BeginFrame(m_Buffer, m_Width, m_Height, m_Format, m_Config);
    m_Active = true;
    m_Pending = false;
}

void Mesh3DPass::Flush()
{
    if (!m_Pending)
    {
        return;
    }
    m_Backend->EndFrame();
    m_Backend->BeginFrame(m_Buffer, m_Width, m_Height, m_Format, m_Config);
    m_Pending = false;
}

void Mesh3DPass::PreExecute(Deki::Object* obj, DekiRendering::RenderContext& ctx)
{
    (void)ctx;
    if (!m_Started && obj)
    {
        Start(obj);
    }
    // A 2D object is about to draw. Anything binned so far sorts before it,
    // so it must reach the framebuffer first.
    if (!m_Active || !m_Pending || !obj)
    {
        return;
    }
    if (obj->GetComponent<MeshComponent>())
    {
        return;  // still in a run of meshes: keep batching
    }
    Flush();
}

void Mesh3DPass::Execute(Deki::Object* obj, DekiRendering::RenderContext& ctx)
{
    (void)ctx;
    if (!m_Started && obj)
    {
        Start(obj);
    }
    if (!m_Active || !obj)
    {
        return;
    }

    MeshComponent* mesh = obj->GetComponent<MeshComponent>();
    if (!mesh)
    {
        return;
    }

    const Mesh3D* geometry = mesh->Resolve();
    if (!geometry)
    {
        return;
    }

#ifdef DEKI_TRANSFORM_3D
    const Deki::Mat4 model = Compose(obj->GetWorldX(), obj->GetWorldY(), obj->GetWorldZ(), obj->GetWorldRotationX(),
                                     obj->GetWorldRotationY(), obj->GetWorldRotation(), obj->GetWorldScaleX(),
                                     obj->GetWorldScaleY(), obj->GetWorldScaleZ());
#else
    const Deki::Mat4 model = Compose(obj->GetWorldX(), obj->GetWorldY(), 0.0f, 0.0f, 0.0f, obj->GetWorldRotation(),
                                     obj->GetWorldScaleX(), obj->GetWorldScaleY(), 1.0f);
#endif

    // Materials come from the mesh, one per `usemtl` group with its own
    // texture. The component's settings go on top: its tint multiplies each
    // material's, its shading model replaces theirs (shading is how you want
    // the object drawn, not part of the model file), and its double-sided
    // switch can only turn double-sided on.
    const Deki3D::MeshAsset* asset = mesh->Asset();
    // 0xAABBGGRR, the order Material3D keeps colours in.
    const uint32_t componentTint = mesh->tintColor.ToABGR8888();

    m_Materials.clear();
    if (asset && asset->MaterialCount() > 0)
    {
        m_Materials.assign(asset->Materials(), asset->Materials() + asset->MaterialCount());
    }
    else
    {
        m_Materials.emplace_back();
    }

    // The component's shader draws every material, where a GPU draws them.
    const DekiRendering::ShaderAsset* shader = mesh->shader.HasGuid() ? mesh->shader.Get() : nullptr;
    const uint32_t shaderColor = mesh->shaderColor.ToABGR8888();
    for (Material3D& material : m_Materials)
    {
        material.tint = MultiplyTint(material.tint, componentTint);
        material.shading = mesh->shading;
        material.doubleSided = material.doubleSided || mesh->doubleSided;
        material.shader = shader;
        material.shaderParams = mesh->shaderParams;
        material.shaderColor = shaderColor;
    }
    if (shader && m_Backend == &m_Raster)
    {
        static bool s_Warned = false;
        if (!s_Warned)
        {
            s_Warned = true;
            DEKI_LOG_WARNING("Mesh3DPass: '%s' has a shader, which only a GPU draws; drawing its shading model",
                             obj->GetName().c_str());
        }
    }

    // Rotation only for normals: uniform scale leaves them pointing the right
    // way, and the shading models here are not worth an inverse transpose.
#ifdef DEKI_TRANSFORM_3D
    const Deki::Mat4 normalMatrix = Mul(RotateZ(obj->GetWorldRotation()),
                                        Mul(RotateY(obj->GetWorldRotationY()), RotateX(obj->GetWorldRotationX())));
#else
    const Deki::Mat4 normalMatrix = RotateZ(obj->GetWorldRotation());
#endif

    m_Backend->DrawMesh(*geometry, m_Materials.data(), static_cast<uint16_t>(m_Materials.size()),
                        Mul(m_ViewProjection, model), normalMatrix);
    m_Pending = true;
}

void Mesh3DPass::EndFrame(DekiRendering::RenderContext& ctx)
{
    (void)ctx;
    if (!m_Active)
    {
        return;
    }
    m_Backend->EndFrame();  // whatever the last run of meshes left binned
    m_Active = false;
    m_Pending = false;
}

}  // namespace Deki3D

// Registers itself with autoAttach, so a project that installs deki-3d gets
// the pass without naming it in its render pipeline. Unregistering on unload
// keeps the factory, which points into this package's code, from outliving
// the DLL.
namespace
{
struct Mesh3DPassRegistrar
{
    Mesh3DPassRegistrar()
    {
        DekiRendering::RenderPassInfo info;
        info.factory = []() -> DekiRendering::RenderPass* { return new Deki3D::Mesh3DPass(); };
        info.autoAttach = true;
        DekiRendering::DekiRenderPassRegistry::Register(Deki3D::Mesh3DPass::kRegistryName, info);
    }
    ~Mesh3DPassRegistrar() { DekiRendering::DekiRenderPassRegistry::Unregister(Deki3D::Mesh3DPass::kRegistryName); }
};
static Mesh3DPassRegistrar s_Mesh3dPassRegistrar;
}  // namespace

// Called from Deki3DInitSystem so a static link keeps this file and the
// registrar above; a firmware link drops a file nothing references.
// Registration itself stays in the static initialiser, which runs before
// deki-rendering attaches the autoAttach passes at startup.
void Deki3DKeepMesh3DPass()
{
}
