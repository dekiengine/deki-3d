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
    if (settings.contains("texture"))
    {
        out.texture = DekiEditor::ReadTextureSettings(settings["texture"]);
    }
    const nlohmann::json model = settings.value("model", nlohmann::json::object());
    if (!model.is_object())
    {
        return out;
    }
    if (model.contains("filter") && model["filter"].is_string())
    {
        out.filter = model["filter"].get<std::string>();
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
        if (it.value().contains("filter") && it.value()["filter"].is_string())
        {
            t.filter = it.value()["filter"].get<std::string>();
        }
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

    // The texture part, in the shape images use.
    nlohmann::json tex = s.value("texture", nlohmann::json::object());
    if (!tex.is_object())
    {
        tex = nlohmann::json::object();
    }
    if (settings.texture.format.empty() || settings.texture.format == "Automatic")
    {
        tex.erase("format");
    }
    else
    {
        tex["format"] = settings.texture.format;
    }
    if (settings.texture.maxSize > 0)
    {
        tex["maxSize"] = settings.texture.maxSize;
    }
    else
    {
        tex.erase("maxSize");
    }
    tex.erase("targets");
    for (const auto& [id, t] : settings.texture.targets)
    {
        DekiEditor::WriteTextureTarget(tex, id, t);
    }
    if (tex.empty())
    {
        s.erase("texture");
    }
    else
    {
        s["texture"] = tex;
    }

    nlohmann::json model = nlohmann::json::object();
    if (!settings.filter.empty() && settings.filter != "Point")
    {
        model["filter"] = settings.filter;
    }
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
        if (!t.filter.empty())
        {
            entry["filter"] = t.filter;
        }
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
