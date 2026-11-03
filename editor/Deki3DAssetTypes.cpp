// Registers .obj as a mesh asset in the editor and compiles it.
//
// The AssetTypeEditor maps ".obj" to the "Mesh" type, which puts models in
// the asset browser and in a MeshComponent's asset picker. The sync handler
// compiles: it reads the source and writes the blob MeshAsset parses into the
// project's asset cache, named by the asset's GUID. The runtime reads it from
// there and the device image packs it.
//
// The parsing lives in ObjImporter, which does not need the editor, so it can
// be tested without one.

#ifdef DEKI_EDITOR

#include "ModelSettings.h"
#include "ObjImporter.h"
#include "../MeshAsset.h"

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/Localization.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki-editor/TextureImporter.h>
#include <deki-editor/TextureFormatResolve.h>
#include <deki-editor/TextureSettings.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/LogSystem.h>
#include <deki/assets/AssetManager.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Deki3D
{
void RegisterModelInspector();  // ModelInspector.cpp
}

namespace DekiEditor
{
namespace
{

class MeshAssetType : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override { return "Mesh"; }
    const char* GetDisplayName() const override { return TrMark("Mesh"); }
    std::vector<std::string> GetExtensions() const override { return { ".obj" }; }
};

REGISTER_EDITOR(MeshAssetType)

/// The name a target is shown by: its platform's, or "the editor".
std::string TargetName(const AssetExportTarget& target)
{
    if (target.platformId.empty())
    {
        return "the editor";
    }
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(target.platformId);
    return platform && !platform->displayName.empty() ? platform->displayName : target.platformId;
}

/// Whether `target` draws meshes on a GPU: a platform with OpenGL, or the
/// editor's own desktop when no platform is chosen.
bool TargetHasGpu(const AssetExportTarget& target)
{
    if (target.platformId.empty())
    {
        return true;
    }
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(target.platformId);
    return platform && platform->Capabilities().count("opengl") != 0;
}

/// The Max Size a model's textures are stored at on `target`: the target's
/// own, else the model's, else none on a GPU and kDefaultMeshTextureSize on a
/// board. 0 is no cap.
int MeshTextureCap(const TextureSettings& settings, const AssetExportTarget& target)
{
    auto it = settings.targets.find(target.platformId);
    if (it != settings.targets.end() && it->second.maxSize >= 0)
    {
        return it->second.maxSize;
    }
    if (settings.maxSize > 0)
    {
        return settings.maxSize;
    }
    return TargetHasGpu(target) ? 0 : Deki3D::kDefaultMeshTextureSize;
}

/// The format a model's texture is stored in for `target`.
Deki3D::TexelFormat ChooseTexelFormat(const TextureSettings& settings, const AssetExportTarget& target, bool hasAlpha)
{
    // TexelFormat is numbered as TextureFormat.
    const TextureFormat f =
        ResolveTextureFormat(settings.format, settings.TargetFormat(target.platformId), target.colorFormat, hasAlpha);
    return static_cast<Deki3D::TexelFormat>(static_cast<uint8_t>(f));
}

std::string Megabytes(uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

/// Whether a compiled mesh fits where `target` loads it: in its external
/// memory, which is where a mesh's vertices, indices and textures go. False,
/// with why, when it cannot. A desktop or other target with an operating
/// system is not checked; a board that names no external memory is said to
/// be unchecked.
bool FitsTarget(const std::vector<uint8_t>& blob, const AssetExportTarget& target, const std::string& name,
                std::string& why, std::vector<std::string>& warnings)
{
    const PlatformConfig* platform =
        target.platformId.empty() ? nullptr : PlatformRegistry::Instance().GetPlatform(target.platformId);
    if (!platform || platform->Capabilities().count("desktop-os") != 0 || blob.size() < sizeof(Deki3D::MeshFileHeader))
    {
        return true;
    }
    Deki3D::MeshFileHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    const uint64_t indexBytes = (header.flags & Deki3D::MeshFileIndex32) ? 4 : 2;
    const uint64_t needs = static_cast<uint64_t>(header.vertexCount) * header.vertexStride +
                           static_cast<uint64_t>(header.indexCount) * indexBytes + header.texturePixelBytes;
    const std::string board = TargetName(target);
    if (platform->externalMemorySize == 0)
    {
        warnings.push_back(name + " takes " + Megabytes(needs) + "; " + board +
                           " does not say how much memory it has, so the build cannot check that it fits");
        return true;
    }
    if (needs > platform->externalMemorySize)
    {
        why = name + " needs " + Megabytes(needs) + " on " + board + ", which has " +
              Megabytes(platform->externalMemorySize) + " of external memory for it. Lower its Max Size or " +
              "Detail for " + board + " in the model's Targets, or leave it out of the scenes this platform builds.";
        return false;
    }
    return true;
}

/// Compiles one .obj as `target` stores it and writes it to `outPath`. False,
/// with why, when it cannot; what the target does without that it still
/// allows goes in `warnings`.
bool CompileObjFile(const std::string& absolutePath, const AssetExportTarget& target, const std::string& outPath,
                    const std::string& name, bool mustFit, std::string& why, std::vector<std::string>& warnings)
{
    std::ifstream in(absolutePath, std::ios::binary);
    if (!in)
    {
        why = name + ": cannot be read";
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();

    // The material library and texture are named relative to the model, and
    // decoding an image is the editor's job, so the parser gets both from
    // here.
    const std::string baseDirectory = fs::path(absolutePath).parent_path().string();
    auto decodeImage = [](const std::string& imagePath, int& width, int& height, std::vector<uint8_t>& rgba)
    {
        DecodedImage decoded;
        if (!DecodeImageFile(imagePath, decoded))
        {
            return false;
        }
        width = decoded.width;
        height = decoded.height;
        rgba = std::move(decoded.rgba);
        return true;
    };
    const Deki3D::ModelSettings settings = Deki3D::ReadModelSettings(absolutePath);
    Deki3D::MeshCompileReport report;
    Deki3D::MeshCompileOptions options;
    options.chooseFormat = [&](bool hasAlpha) { return ChooseTexelFormat(settings.texture, target, hasAlpha); };
    options.maxTextureSize = MeshTextureCap(settings.texture, target);
    options.smoothTextures = settings.SmoothFor(target.platformId);
    options.detail = settings.DetailFor(target.platformId);
    options.report = &report;

    std::vector<uint8_t> blob;
    std::string error;
    if (!Deki3D::CompileObjToMesh(text.str(), baseDirectory, decodeImage, blob, error, options))
    {
        why = name + " did not compile: " + error;
        return false;
    }
    const std::string where = TargetName(target);
    if (options.smoothTextures && !TargetHasGpu(target))
    {
        warnings.push_back(name + ": Smooth filtering needs a GPU; " + where + " draws its textures unfiltered");
    }
    // A reduction that stops well above what Detail asks says so.
    const double asked = static_cast<double>(report.trianglesIn) * options.detail;
    if (options.detail < 1.0f && report.trianglesOut > asked * 1.1 + 1.0)
    {
        char line[256];
        std::snprintf(line, sizeof(line),
                      ": Detail %d%% on %s asks for %u of its %u triangles; it keeps %u (%d%%), since its seams, open "
                      "edges and material borders stay where they are",
                      static_cast<int>(options.detail * 100.0f + 0.5f), where.c_str(),
                      static_cast<unsigned>(asked + 0.5), report.trianglesIn, report.trianglesOut,
                      static_cast<int>(100.0 * report.trianglesOut / std::max(1u, report.trianglesIn) + 0.5));
        warnings.push_back(name + line);
    }
    std::string fit;
    if (!FitsTarget(blob, target, name, fit, warnings))
    {
        if (mustFit)
        {
            why = fit;
            return false;
        }
        warnings.push_back(fit);
    }

    std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        why = name + ": cannot write the compiled mesh to " + outPath;
        return false;
    }
    out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    DEKI_LOG_DEBUG("Deki3D: compiled '%s' to %zu bytes", fs::path(absolutePath).filename().string().c_str(),
                   blob.size());
    return true;
}

/// Compiles one .obj into the cache for the editor's target. Failure leaves
/// no cache file, so the component's asset stays unresolved and the pass draws
/// nothing rather than something wrong.
void HandleObjSync(const std::string& absolutePath, const std::string& guid, const std::string& projectPath)
{
    const AssetPipeline* pipeline = AssetPipeline::Instance();
    const AssetExportTarget target = pipeline ? pipeline->GetEditorTarget() : AssetExportTarget{};
    const fs::path cachePath = fs::path(GetCacheDirectory(projectPath)) / guid;
    // The editor shows what the active platform gets, even a mesh too big for
    // it, which is said here and stops that platform's build.
    const std::string name = fs::path(absolutePath).filename().string();
    std::string why;
    std::vector<std::string> warnings;
    const bool compiled = CompileObjFile(absolutePath, target, cachePath.string(), name, false, why, warnings);
    for (const std::string& warning : warnings)
    {
        DEKI_LOG_WARNING("Deki3D: %s", warning.c_str());
    }
    if (!compiled)
    {
        DEKI_LOG_WARNING("Deki3D: %s", why.c_str());
        return;
    }

    // Point the asset manager at the compiled blob, cached under the source's
    // GUID. Otherwise the GUID resolves to the .obj text, which MeshAsset
    // rejects.
    if (auto* assets = Deki::AssetManager::Get())
    {
        assets->RegisterGuid(guid, guid);
    }
}

/// Points every already-compiled model at its cache entry. The sync handler
/// above runs only when a source changes, so a project that opens with a warm
/// cache needs this to register its GUIDs.
void RegisterCompiledMeshes(AssetPipeline* pipeline, const std::string& projectPath)
{
    auto* assets = Deki::AssetManager::Get();
    if (!pipeline || !assets)
    {
        return;
    }

    const fs::path cacheDir(GetCacheDirectory(projectPath));
    for (const auto& entry : pipeline->GetAllAssets())
    {
        const std::string& relativePath = entry.first;
        if (relativePath.size() < 4 || relativePath.compare(relativePath.size() - 4, 4, ".obj") != 0)
        {
            continue;
        }

        const std::string& guid = entry.second.guid;
        std::error_code ec;
        if (!guid.empty() && fs::exists(cacheDir / guid, ec))
        {
            assets->RegisterGuid(guid, guid);
        }
    }
}

// A static initialiser, not work in the package entry point, so the vtable of
// the AssetTypeEditor above stays stable across package rebuilds (see the
// note in EditorExtension.h).
struct Deki3DAssetRegistrar
{
    Deki3DAssetRegistrar()
    {
        AssetTypeRegistry::Instance().RegisterCategory(".obj", AssetCategory::Data);
        Deki3D::RegisterModelInspector();

        AssetPipeline::OnStarted(
            [](AssetPipeline* pipeline)
            {
                pipeline->RegisterSyncHandler(".obj", HandleObjSync);
                pipeline->RegisterExportEncoder(".obj",
                                                [](const AssetExportContext& ctx)
                                                {
                                                    return CompileObjFile(ctx.absolutePath, ctx.target, ctx.outPath,
                                                                          ctx.relativePath, true, *ctx.error,
                                                                          *ctx.warnings);
                                                });
            });

        AssetPipeline::OnImportComplete([](AssetPipeline* pipeline)
                                        { RegisterCompiledMeshes(pipeline, pipeline->GetProjectPath()); });
    }
};
static Deki3DAssetRegistrar s_Deki3DAssetRegistrar;

}  // namespace
}  // namespace DekiEditor

#endif  // DEKI_EDITOR
