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

/// How one of a model's textures is stored on `target`: as its image says,
/// the image's Format, Max Size and filter there (the image's inspector),
/// the same settings a sprite of it follows.
Deki3D::MeshTextureChoice TextureChoiceFor(const std::string& imagePath, const AssetExportTarget& target)
{
    const TextureSettings image = ReadImageTextureSettings(imagePath);
    Deki3D::MeshTextureChoice choice;
    choice.maxSize = image.MaxSizeFor(target.platformId);
    choice.filter = ResolveTextureFilter(image, target.platformId);
    choice.format = [image, target](bool hasAlpha)
    {
        // TexelFormat is numbered as TextureFormat.
        const TextureFormat f =
            ResolveTextureFormat(image.format, image.TargetFormat(target.platformId), target.colorFormat, hasAlpha);
        return static_cast<Deki3D::TexelFormat>(static_cast<uint8_t>(f));
    };
    return choice;
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
              "Detail for " + board + " (its textures' Max Size in their images, the mesh's Detail in the model), " +
              "or leave it out of the scenes this platform builds.";
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
    options.textureFor = [&](const std::string& imagePath) { return TextureChoiceFor(imagePath, target); };
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
                // Its material library and texture images: a change to one,
                // or to an image's settings, rebuilds the model.
                pipeline->RegisterSourceFilesReporter(".obj",
                                                      [](const std::string& absolutePath)
                                                      {
                                                          // Up to the mtllib line only, near the top:
                                                          // a stamp is made often, and models are big.
                                                          std::ifstream in(absolutePath, std::ios::binary);
                                                          std::string text, line;
                                                          while (std::getline(in, line))
                                                          {
                                                              text += line + "\n";
                                                              if (line.rfind("mtllib", 0) == 0)
                                                              {
                                                                  break;
                                                              }
                                                          }
                                                          return Deki3D::ObjSourceFiles(
                                                              text, fs::path(absolutePath).parent_path().string());
                                                      });
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
