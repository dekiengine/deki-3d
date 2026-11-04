#include "ModelSettings.h"

#ifdef DEKI_EDITOR

#include <nlohmann/json.hpp>

#include <fstream>

namespace Deki3D
{
namespace
{

nlohmann::json ReadSidecar(const std::string& absolutePath)
{
    std::ifstream in(absolutePath + ".data");
    if (!in.is_open())
    {
        return nlohmann::json::object();
    }
    try
    {
        nlohmann::json data = nlohmann::json::parse(in);
        return data.is_object() ? data : nlohmann::json::object();
    }
    catch (const nlohmann::json::exception&)
    {
        return nlohmann::json::object();
    }
}

float ReadDetail(const nlohmann::json& j, float fallback)
{
    if (j.contains("detail") && j["detail"].is_number())
    {
        const float d = j["detail"].get<float>();
        return d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
    }
    return fallback;
}

}  // namespace

ModelSettings ReadModelSettings(const std::string& absolutePath)
{
    ModelSettings out;
    const nlohmann::json data = ReadSidecar(absolutePath);
    const nlohmann::json settings = data.value("settings", nlohmann::json::object());
    if (!settings.is_object())
    {
        return out;
    }
    const nlohmann::json model = settings.value("model", nlohmann::json::object());
    if (!model.is_object())
    {
        return out;
    }
    out.detail = ReadDetail(model, 1.0f);
    const nlohmann::json targets = model.value("targets", nlohmann::json::object());
    for (auto it = targets.begin(); targets.is_object() && it != targets.end(); ++it)
    {
        if (!it.value().is_object())
        {
            continue;
        }
        ModelTargetSettings t;
        t.detail = ReadDetail(it.value(), -1.0f);
        if (!t.Empty())
        {
            out.targets[it.key()] = t;
        }
    }
    return out;
}

bool WriteModelSettings(const std::string& absolutePath, const ModelSettings& settings)
{
    nlohmann::json data = ReadSidecar(absolutePath);
    if (!data.contains("settings") || !data["settings"].is_object())
    {
        data["settings"] = nlohmann::json::object();
    }
    nlohmann::json& s = data["settings"];

    // Its textures' settings are their images' now; a model saved with its
    // own (format, Max Size, filter) drops them, rather than keep settings
    // nothing reads.
    s.erase("texture");

    nlohmann::json model = nlohmann::json::object();
    if (settings.detail < 1.0f)
    {
        model["detail"] = settings.detail;
    }
    for (const auto& [id, t] : settings.targets)
    {
        if (t.Empty())
        {
            continue;
        }
        nlohmann::json entry = nlohmann::json::object();
        if (t.detail >= 0.0f)
        {
            entry["detail"] = t.detail;
        }
        model["targets"][id] = entry;
    }
    if (model.empty())
    {
        s.erase("model");
    }
    else
    {
        s["model"] = model;
    }

    std::ofstream out(absolutePath + ".data", std::ios::trunc);
    if (!out.is_open())
    {
        return false;
    }
    out << data.dump(2);
    return static_cast<bool>(out);
}

}  // namespace Deki3D

#endif  // DEKI_EDITOR
