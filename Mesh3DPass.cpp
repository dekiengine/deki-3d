#include "Mesh3DPass.h"
#include "Deki3DInit.h"

#include "Mesh3DSettings.h"
#include "Math3D.h"
#include "MeshComponent.h"

#include "deki-rendering/CameraComponent.h"
#include "deki-rendering/DekiRenderer.h"  // DekiRendering::RenderContext
#include "deki-rendering/DekiRenderPassRegistry.h"
#include "deki-rendering/Light.h"
#include "deki-rendering/QuadBlit.h"  // NoteWritten

#include <deki/LogSystem.h>
#include <deki/Object.h>
#include <deki/Scene.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

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

/// The object's world transform as a matrix.
Deki::Mat4 ModelOf(const Deki::Object* obj)
{
#ifdef DEKI_TRANSFORM_3D
    return Compose(obj->GetWorldX(), obj->GetWorldY(), obj->GetWorldZ(), obj->GetWorldRotationX(),
                   obj->GetWorldRotationY(), obj->GetWorldRotation(), obj->GetWorldScaleX(), obj->GetWorldScaleY(),
                   obj->GetWorldScaleZ());
#else
    return Compose(obj->GetWorldX(), obj->GetWorldY(), 0.0f, 0.0f, 0.0f, obj->GetWorldRotation(),
                   obj->GetWorldScaleX(), obj->GetWorldScaleY(), 1.0f);
#endif
}

/// Every active object below `object`, itself included, in depth-first order.
template <typename Visit>
void VisitActive(const Deki::Object* object, Visit visit)
{
    if (!object || !object->IsActive())
    {
        return;
    }
    visit(object);
    for (const Deki::Object* child : object->GetChildren())
    {
        VisitActive(child, visit);
    }
}

bool IsPerspective(const DekiRendering::CameraComponent& c)
{
    return c.projection == Deki::ProjectionMode::Perspective;
}

}  // namespace

Mesh3DPass::~Mesh3DPass()
{
#ifdef DEKI_CAPABILITY_OPENGL
    if (m_Gpu)
    {
        DestroyGpuBackend(m_Gpu);
    }
#endif
}

Mesh3DBackend* Mesh3DPass::Gpu()
{
#ifdef DEKI_CAPABILITY_OPENGL
    if (!m_GpuTried)
    {
        m_GpuTried = true;
        m_Gpu = CreateGpuBackend();
        if (m_Gpu)
        {
            DEKI_LOG_INFO("Mesh3DPass: drawing meshes with %s", GpuBackendName());
        }
    }
#endif
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
    m_Config.tileSize = cam3d.TileSize();
    m_Config.perspectiveCorrect = cam3d.perspectiveCorrect;
    m_Config.vertexSnap = cam3d.vertexSnap;

    GatherLights(sceneObject);
    PlanShadows(sceneObject);
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
                DEKI_LOG_WARNING("Mesh3DPass: the scene asks for the GPU, but this platform has no OpenGL; drawing "
                                 "in software");
            }
        }
    }

    m_Backend->BeginFrame(m_Buffer, m_Width, m_Height, m_Format, m_Config);
    m_Active = true;
    m_Pending = false;
    DrawShadows();
}

void Mesh3DPass::PlanShadows(const Deki::Object* sceneObject)
{
    m_Config.shadowCount = 0;
    m_Casters.clear();
    if (m_ShadowLights.empty())
    {
        return;
    }

    // The casters, and the box round them all in the world.
    Deki::Vector3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    auto take = [&](const Deki::Object* obj)
    {
        const MeshComponent* mesh = obj->GetComponent<MeshComponent>();
        const Mesh3D* geometry = mesh && mesh->castShadows ? mesh->Resolve() : nullptr;
        if (!geometry)
        {
            return;
        }
        const Deki::Mat4 model = ModelOf(obj);
        m_Casters.push_back({ mesh, model });
        for (int corner = 0; corner < 8; ++corner)
        {
            const Deki::Vector3 p((corner & 1) ? geometry->boundsMax.x : geometry->boundsMin.x,
                                  (corner & 2) ? geometry->boundsMax.y : geometry->boundsMin.y,
                                  (corner & 4) ? geometry->boundsMax.z : geometry->boundsMin.z);
            float w = 1.0f;
            const Deki::Vector3 at = TransformPoint(model, p, w);
            lo = Deki::Vector3(std::min(lo.x, at.x), std::min(lo.y, at.y), std::min(lo.z, at.z));
            hi = Deki::Vector3(std::max(hi.x, at.x), std::max(hi.y, at.y), std::max(hi.z, at.z));
        }
    };
    if (const Deki::Scene* scene = sceneObject->GetOwnerScene())
    {
        for (const Deki::Object* root : scene->GetObjects())
        {
            VisitActive(root, take);
        }
    }
    if (m_Casters.empty())
    {
        return;
    }
    const Deki::Vector3 centre = (lo + hi) * 0.5f;
    const float radius = std::max((hi - lo).Length() * 0.5f, 0.01f);

    for (const ShadowLight& shadowLight : m_ShadowLights)
    {
        Light3D& light = m_Config.lights[shadowLight.light];
        ShadowMap& map = m_Config.shadows[m_Config.shadowCount];
        const Deki::Vector3 dir = light.direction.Normalized();
        const Deki::Vector3 up =
            std::fabs(dir.y) > 0.99f ? Deki::Vector3(0.0f, 0.0f, 1.0f) : Deki::Vector3(0.0f, 1.0f, 0.0f);
        if (light.kind == Light3D::Kind::Directional)
        {
            // The sun's view of a box holding every caster, from beyond it.
            const Deki::Vector3 eye = centre - dir * (radius * 2.0f);
            map.viewProjection =
                Mul(Ortho(-radius, radius, -radius, radius, radius * 0.5f, radius * 3.5f), LookAt(eye, centre, up));
        }
        else
        {
            // The spot's own view, as wide as its cone, out to its range.
            const float cone = 2.0f * std::acos(std::min(std::max(light.cosOuter, -1.0f), 1.0f)) + 0.05f;
            map.viewProjection = Mul(Perspective(std::min(cone, 3.0f), 1.0f, std::max(light.range * 0.01f, 0.05f),
                                                 light.range),
                                     LookAt(light.position, light.position + dir, up));
        }
        map.resolution = static_cast<int>(shadowLight.resolution);
        map.bias = shadowLight.bias;
        light.shadow = m_Config.shadowCount++;
    }
}

void Mesh3DPass::DrawShadows()
{
    if (m_Config.shadowCount == 0)
    {
        return;
    }
    if (!m_Backend->DrawsShadows())
    {
        static bool s_Told = false;
        if (!s_Told)
        {
            s_Told = true;
            DEKI_LOG_WARNING("Lights: the scene has lights that cast shadows, which only the GPU draws; the software "
                             "rasteriser draws them without");
        }
        return;
    }
    for (int slot = 0; slot < m_Config.shadowCount; ++slot)
    {
        for (const Caster& caster : m_Casters)
        {
            const Mesh3D* geometry = caster.mesh->Resolve();
            if (!geometry)
            {
                continue;
            }
            BuildMaterials(*caster.mesh, ShadingModel::Unlit);
            m_Backend->DrawShadowCaster(slot, *geometry, m_Materials.data(), static_cast<uint16_t>(m_Materials.size()),
                                        caster.model);
        }
    }
}

void Mesh3DPass::GatherLights(const Deki::Object* sceneObject)
{
    static std::vector<const DekiRendering::Light*> s_Found;
    DekiRendering::Light::InScene(sceneObject->GetOwnerScene(), s_Found);
    m_Lit = !s_Found.empty();
    m_Config.ambient = Deki::Vector3(0.0f, 0.0f, 0.0f);
    m_Config.lightCount = 0;
    m_ShadowLights.clear();

    // The ambient adds up; the rest are lights of their own, the strongest
    // kMaxLights of them if there are more.
    static std::vector<std::pair<float, const DekiRendering::Light*>> s_Lights;
    s_Lights.clear();
    for (const DekiRendering::Light* light : s_Found)
    {
        if (!light->Shines())
        {
            continue;
        }
        const Deki::Vector3 color(light->color.r / 255.0f * light->intensity, light->color.g / 255.0f * light->intensity,
                                  light->color.b / 255.0f * light->intensity);
        if (light->type == DekiRendering::LightType::Ambient)
        {
            m_Config.ambient = m_Config.ambient + color;
        }
        else
        {
            s_Lights.emplace_back(color.x + color.y + color.z, light);
        }
    }
    std::stable_sort(s_Lights.begin(), s_Lights.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    for (size_t i = 0; i < s_Lights.size(); ++i)
    {
        const DekiRendering::Light& light = *s_Lights[i].second;
        if (i >= static_cast<size_t>(RasterConfig::kMaxLights))
        {
            static std::unordered_set<std::string> s_Told;
            const std::string& name = light.GetOwner()->GetName();
            if (s_Told.insert(name).second)
            {
                DEKI_LOG_WARNING("Lights: '%s' does not light the meshes: they take the %d strongest lights, and it is "
                                 "not one of them",
                                 name.c_str(), RasterConfig::kMaxLights);
            }
            continue;
        }
        const Deki::Object* owner = light.GetOwner();
        // Shadows: the sun's and a spot's, the first two of them.
        if (light.castShadows)
        {
            if (light.type == DekiRendering::LightType::Point)
            {
                static std::unordered_set<std::string> s_ToldPoint;
                if (s_ToldPoint.insert(owner->GetName()).second)
                {
                    DEKI_LOG_WARNING("Lights: '%s' is a point light, which casts no shadows yet",
                                     owner->GetName().c_str());
                }
            }
            else if (m_ShadowLights.size() < static_cast<size_t>(RasterConfig::kMaxShadows))
            {
                m_ShadowLights.push_back({ m_Config.lightCount, static_cast<float>(light.shadowResolution),
                                           light.shadowBias });
            }
            else
            {
                static std::unordered_set<std::string> s_ToldMany;
                if (s_ToldMany.insert(owner->GetName()).second)
                {
                    DEKI_LOG_WARNING("Lights: '%s' casts no shadows: at most %d lights cast them in a frame",
                                     owner->GetName().c_str(), RasterConfig::kMaxShadows);
                }
            }
        }
        Light3D& l = m_Config.lights[m_Config.lightCount++];
        l = Light3D{};
        l.color = Deki::Vector3(light.color.r / 255.0f * light.intensity, light.color.g / 255.0f * light.intensity,
                                light.color.b / 255.0f * light.intensity);
        if (light.type == DekiRendering::LightType::Directional)
        {
            // Yaw and pitch rather than a vector, so an author can swing the
            // light around without normalising by hand. Yaw 0 puts it behind
            // the viewer.
            const float yaw = light.yaw * kPi / 180.0f;
            const float pitch = light.pitch * kPi / 180.0f;
            l.kind = Light3D::Kind::Directional;
            l.direction =
                Deki::Vector3(std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
            continue;
        }
#ifdef DEKI_TRANSFORM_3D
        l.position = Deki::Vector3(owner->GetWorldX(), owner->GetWorldY(), owner->GetWorldZ());
#else
        l.position = Deki::Vector3(owner->GetWorldX(), owner->GetWorldY(), 0.0f);
#endif
        l.range = light.range;
        if (light.type == DekiRendering::LightType::Point)
        {
            l.kind = Light3D::Kind::Point;
            continue;
        }
        // A spot faces along its object's X axis, as in 2D, where that is
        // the way its rotation turns it.
        l.kind = Light3D::Kind::Spot;
        l.direction = TransformDirection(CameraRotation(owner), Deki::Vector3(1.0f, 0.0f, 0.0f));
        const float half = std::min(std::max(light.spotAngle, 1.0f), 179.0f) * 0.5f * kPi / 180.0f;
        const float soft = std::min(std::max(light.spotSoftness, 0.0f), 1.0f);
        l.cosOuter = std::cos(half);
        l.cosInner = std::max(std::cos(half * (1.0f - soft)), l.cosOuter + 1e-4f);
    }
}

void Mesh3DPass::EndBatch()
{
    m_Backend->EndFrame();
    // Where the 2D is drawn on the GPU, what the batch wrote into the buffer
    // goes over the 2D before it and under the 2D after.
    if (m_Pending && m_Backend->WritesBuffer())
    {
        QuadBlit::NoteWritten(m_Buffer);
    }
    m_Pending = false;
}

void Mesh3DPass::Flush()
{
    if (!m_Pending)
    {
        return;
    }
    EndBatch();
    m_Backend->BeginFrame(m_Buffer, m_Width, m_Height, m_Format, m_Config);
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

    const Deki::Mat4 model = ModelOf(obj);

    // A scene without a Light is not lit: a lit mesh draws in its own
    // colours, and says why once.
    ShadingModel shading = mesh->shading;
    if (!m_Lit && shading != ShadingModel::Unlit)
    {
        shading = ShadingModel::Unlit;
        static std::unordered_set<std::string> s_Told;
        if (s_Told.insert(obj->GetName()).second)
        {
            DEKI_LOG_WARNING("Lights: '%s' is lit, but the scene has no Light; it draws unlit until one is added",
                             obj->GetName().c_str());
        }
    }
    BuildMaterials(*mesh, shading);
    const DekiRendering::ShaderAsset* shader = mesh->shader.HasGuid() ? mesh->shader.Get() : nullptr;
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

    // The frame's clear before anything of the mesh is written: a frame of
    // more than one batch fills while its meshes are drawn. The software
    // rasteriser takes it as one pixel and draws it under each tile as it
    // fills it, which saves the clear's own pass over the screen.
    if (m_Backend->WritesBuffer() && QuadBlit::PendingClear())
    {
        uint8_t pixel[4];
        if (m_Backend == &m_Raster && QuadBlit::TakePendingClear(m_Buffer, pixel))
        {
            m_Raster.SetBackground(pixel);
        }
        else
        {
            QuadBlit::BeforeWriting(m_Buffer);
        }
    }

    m_Backend->DrawMesh(*geometry, m_Materials.data(), static_cast<uint16_t>(m_Materials.size()),
                        Mul(m_ViewProjection, model), normalMatrix, model);
    m_Pending = true;
}

void Mesh3DPass::BuildMaterials(const MeshComponent& mesh, ShadingModel shading)
{
    // Materials come from the mesh, one per `usemtl` group with its own
    // texture. The component's settings go on top: its tint multiplies each
    // material's, its shading model replaces theirs (shading is how you want
    // the object drawn, not part of the model file), and its double-sided
    // switch can only turn double-sided on.
    const Deki3D::MeshAsset* asset = mesh.Asset();
    // 0xAABBGGRR, the order Material3D keeps colours in.
    const uint32_t componentTint = mesh.tintColor.ToABGR8888();

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
    const DekiRendering::ShaderAsset* shader = mesh.shader.HasGuid() ? mesh.shader.Get() : nullptr;
    const uint32_t shaderColor = mesh.shaderColor.ToABGR8888();
    for (Material3D& material : m_Materials)
    {
        material.tint = MultiplyTint(material.tint, componentTint);
        material.shading = shading;
        material.doubleSided = material.doubleSided || mesh.doubleSided;
        material.shader = shader;
        material.shaderParams = mesh.shaderParams;
        material.shaderColor = shaderColor;
    }
}

void Mesh3DPass::EndFrame(DekiRendering::RenderContext& ctx)
{
    (void)ctx;
    if (!m_Active)
    {
        return;
    }
    EndBatch();  // whatever the last run of meshes left binned
    m_Active = false;
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
