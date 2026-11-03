#pragma once

#ifdef DEKI_EDITOR

// A model's settings, kept in its sidecar (<model.obj>.data) under
// "settings":
//
//   "texture": its textures' format and Max Size, the shape an image's has
//              (TextureSettings.h), per target too. A model's Max Size 0 is
//              Automatic: no cap where the target has a GPU,
//              kDefaultMeshTextureSize on a board.
//   "model":   { "filter": "Point" | "Smooth",
//                "detail": the share of triangles kept, 1 (or absent) for all,
//                "targets": { "<platformId>": { "filter": ..., "detail": ... } } }
//
// Read by the compile for each target, and written by the Model inspector.

#include <deki-editor/TextureSettings.h>

#include <map>
#include <string>

namespace Deki3D
{

/// One target's own model values. Empty filter and detail < 0: as Default.
struct ModelTargetSettings
{
    std::string filter;
    float detail = -1.0f;

    bool Empty() const { return filter.empty() && detail < 0.0f; }
};

struct ModelSettings
{
    DekiEditor::TextureSettings texture;
    std::string filter;   // "" (Point) or "Smooth"
    float detail = 1.0f;  // share of triangles kept
    std::map<std::string, ModelTargetSettings> targets;

    bool SmoothFor(const std::string& platformId) const
    {
        auto it = targets.find(platformId);
        const std::string& f = (it != targets.end() && !it->second.filter.empty()) ? it->second.filter : filter;
        return f == "Smooth";
    }

    float DetailFor(const std::string& platformId) const
    {
        auto it = targets.find(platformId);
        return (it != targets.end() && it->second.detail >= 0.0f) ? it->second.detail : detail;
    }
};

/// The settings in `<absolutePath>.data`; defaults when it has none.
ModelSettings ReadModelSettings(const std::string& absolutePath);

/// Writes `settings` into `<absolutePath>.data`, keeping everything else in
/// it. False when the file cannot be written.
bool WriteModelSettings(const std::string& absolutePath, const ModelSettings& settings);

}  // namespace Deki3D

#endif  // DEKI_EDITOR
