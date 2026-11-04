#pragma once

#ifdef DEKI_EDITOR

// A model's settings, kept in its sidecar (<model.obj>.data) under
// "settings":
//
//   "model": { "detail": the share of triangles kept, 1 (or absent) for all,
//              "targets": { "<platformId>": { "detail": ... } } }
//
// Its textures are images, stored and drawn as each image's own settings say
// (TextureSettings.h, the image's inspector): a model has none of its own.
//
// Read by the compile for each target, and written by the Model inspector.

#include <map>
#include <string>

namespace Deki3D
{

/// One target's own model values. detail < 0: as Default.
struct ModelTargetSettings
{
    float detail = -1.0f;

    bool Empty() const { return detail < 0.0f; }
};

struct ModelSettings
{
    float detail = 1.0f;  // share of triangles kept
    std::map<std::string, ModelTargetSettings> targets;

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
