/**
 * @file Camera3DMigration.cpp
 * @brief Moves a pre-0.18 Camera3DComponent's lens onto the scene camera.
 *
 * Before 0.18 the perspective projection lived on Camera3DComponent, beside the
 * 2D camera. One camera now holds the whole view (Projection, field of view,
 * clip planes) and the old component stays as Mesh3DSettings with the rest. On
 * an object that has both, the field of view and clip planes move to the
 * camera, which becomes Perspective; a key the old file left out had the old
 * component's default, so that default is what moves. The next save writes
 * the new shape.
 */

#ifdef DEKI_EDITOR

#include <deki/LogSystem.h>
#include <deki/SceneMigration.h>
#include <nlohmann/json.hpp>

#include <string>

namespace
{

bool IsOldCamera3D(const std::string& type)
{
    return type == "Deki3D::Camera3DComponent" || type == "Camera3DComponent";
}

bool IsCamera(const std::string& type)
{
    return type == "DekiRendering::CameraComponent" || type == "CameraComponent";
}

void MigrateCamera3D(nlohmann::json& components)
{
    nlohmann::json* old3d = nullptr;
    nlohmann::json* camera = nullptr;
    for (auto& comp : components)
    {
        if (!comp.is_object())
            continue;
        const std::string type = comp.value("type", "");
        if (IsOldCamera3D(type))
            old3d = &comp;
        else if (IsCamera(type))
            camera = &comp;
    }
    if (!old3d)
        return;

    nlohmann::json& oldProps = (*old3d)["properties"];
    if (!oldProps.is_object())
        oldProps = nlohmann::json::object();
    (*old3d)["type"] = "Deki3D::Mesh3DSettings";

    if (!camera)
    {
        // The lens has nowhere to go on this object. Drop it rather than keep
        // keys Mesh3DSettings no longer has, and say which scene camera to set.
        if (oldProps.contains("fieldOfView") || oldProps.contains("nearPlane") || oldProps.contains("farPlane"))
            DEKI_LOG_WARNING("A Camera3DComponent on an object without a camera became Mesh3DSettings; set the "
                             "scene camera's Projection to Perspective and its field of view by hand");
        oldProps.erase("fieldOfView");
        oldProps.erase("nearPlane");
        oldProps.erase("farPlane");
        return;
    }

    nlohmann::json& camProps = (*camera)["properties"];
    if (!camProps.is_object())
        camProps = nlohmann::json::object();
    const struct { const char* key; float oldDefault; } lens[] = {
        { "fieldOfView", 60.0f }, { "nearPlane", 0.1f }, { "farPlane", 100.0f } };
    for (const auto& l : lens)
    {
        if (!camProps.contains(l.key))
            camProps[l.key] = oldProps.contains(l.key) ? oldProps[l.key] : nlohmann::json(l.oldDefault);
        oldProps.erase(l.key);
    }
    camProps["projection"] = "Perspective";
}

Deki::ComponentsMigrationRegistrar s_Camera3DMigration(&MigrateCamera3D);

}  // namespace

#endif  // DEKI_EDITOR
