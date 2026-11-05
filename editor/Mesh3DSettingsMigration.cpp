// Editor-only translation unit (a firmware build compiles nothing from it).
#ifdef DEKI_EDITOR

// Mesh3DSettings once held the scene's one light: lightYaw, lightPitch and
// ambient, lighting a face by ambient + facing x (1 - ambient). The light is
// the Light component's now, so a scene saved before is moved onto two
// Lights on the same object, an Ambient one of `ambient` and a Directional
// one of 1 - ambient at that yaw and pitch, which light exactly as before.
//
// And its tile size was one number for every platform, saved as 32 unless
// changed. It is the platform's own now (`tiles` Platform), unless the scene
// fixes one, so a scene saved before with the old default 32 takes the
// platform's, as a new scene does; one that chose another size keeps it,
// Fixed. Only the speed can change: the picture is the same at any size.
//
// Device scenes are compiled from the migrated JSON.

#include <deki/SceneMigration.h>

#include <nlohmann/json.hpp>

namespace
{

void MigrateMesh3DSettingsLight(nlohmann::json& components)
{
    nlohmann::json lights = nlohmann::json::array();
    for (nlohmann::json& component : components)
    {
        if (!component.is_object() || component.value("type", std::string()) != "Deki3D::Mesh3DSettings")
        {
            continue;
        }
        nlohmann::json& props = component["properties"];
        if (!props.is_object() || !(props.contains("lightYaw") || props.contains("lightPitch") ||
                                    props.contains("ambient")))
        {
            continue;
        }
        // The old defaults where a field was not saved.
        const float yaw = props.value("lightYaw", -25.0f);
        const float pitch = props.value("lightPitch", 55.0f);
        const float ambient = props.value("ambient", 0.25f);
        props.erase("lightYaw");
        props.erase("lightPitch");
        props.erase("ambient");
        lights.push_back(
            { { "type", "DekiRendering::Light" }, { "properties", { { "type", "Ambient" }, { "intensity", ambient } } } });
        lights.push_back({ { "type", "DekiRendering::Light" },
                           { "properties",
                             { { "type", "Directional" },
                               { "intensity", 1.0f - ambient },
                               { "yaw", yaw },
                               { "pitch", pitch } } } });
    }
    for (nlohmann::json& light : lights)
    {
        components.push_back(std::move(light));
    }
}

Deki::ComponentsMigrationRegistrar s_Mesh3DSettingsLight(&MigrateMesh3DSettingsLight);

void MigrateMesh3DSettingsTiles(nlohmann::json& components)
{
    for (nlohmann::json& component : components)
    {
        if (!component.is_object() || component.value("type", std::string()) != "Deki3D::Mesh3DSettings")
        {
            continue;
        }
        nlohmann::json& props = component["properties"];
        // A scene saved since has `tiles`, whatever its size.
        if (!props.is_object() || props.contains("tiles"))
        {
            continue;
        }
        const nlohmann::json size = props.value("tileSize", nlohmann::json(32));
        props["tiles"] = size.is_number() && size.get<double>() != 32.0 ? "Fixed" : "Platform";
    }
}

Deki::ComponentsMigrationRegistrar s_Mesh3DSettingsTiles(&MigrateMesh3DSettingsTiles);

}  // namespace

#endif  // DEKI_EDITOR
