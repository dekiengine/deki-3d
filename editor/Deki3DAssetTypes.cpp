/**
 * @file Deki3DAssetTypes.cpp
 * @brief Teaches the editor that .obj is a mesh, and compiles it.
 *
 * Two registrations. The AssetTypeEditor makes ".obj" resolve to the "Mesh"
 * type, which is what puts models in the asset browser and lets a
 * MeshComponent's asset picker offer them. The sync handler is the compile
 * step: it reads the source and writes the blob MeshAsset parses into the
 * project's asset cache, named by the asset's GUID, which is where the
 * runtime looks and what the device image packs.
 *
 * The parsing itself lives in ObjImporter, free of the editor, so it can be
 * tested without one.
 */

#ifdef DEKI_EDITOR

#include "ObjImporter.h"

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki-editor/TextureImporter.h>
#include <deki-editor/TextureFormatResolve.h>
#include <deki/LogSystem.h>
#include <deki/assets/AssetManager.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>


namespace fs = std::filesystem;

namespace DekiEditor
{
namespace
{

class MeshAssetType : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override { return "Mesh"; }
    const char* GetDisplayName() const override { return "Mesh"; }
    std::vector<std::string> GetExtensions() const override { return { ".obj" }; }
};

REGISTER_EDITOR(MeshAssetType)

/// The format a model's texture is stored in for `target`: the model's
/// sidecar (<model.obj>.data, settings.texture: "format" and "targets", as for
/// an image) or Automatic.
Deki3D::TexelFormat ChooseTexelFormat(const std::string& absolutePath, const AssetExportTarget& target,
                                      bool hasAlpha)
{
    std::string assetFormat, targetFormat;
    std::ifstream dataFile(absolutePath + ".data");
    if (dataFile.is_open())
    {
        try
        {
            const nlohmann::json data = nlohmann::json::parse(dataFile);
            if (data.contains("settings") && data["settings"].contains("texture"))
            {
                const auto& tex = data["settings"]["texture"];
                assetFormat = tex.value("format", std::string());
                if (tex.contains("targets") && tex["targets"].contains(target.platformId) &&
                    tex["targets"][target.platformId].is_string())
                    targetFormat = tex["targets"][target.platformId].get<std::string>();
            }
        }
        catch (const nlohmann::json::exception&)
        {
        }
    }
    // TexelFormat is numbered as TextureFormat.
    const TextureFormat f = ResolveTextureFormat(assetFormat, targetFormat, target.colorFormat, hasAlpha);
    return static_cast<Deki3D::TexelFormat>(static_cast<uint8_t>(f));
}

/// Compile one .obj as `target` stores it and write it to `outPath`. Failure
/// is logged and writes nothing.
bool CompileObjFile(const std::string& absolutePath, const AssetExportTarget& target, const std::string& outPath)
{
    std::ifstream in(absolutePath, std::ios::binary);
    if (!in)
    {
        DEKI_LOG_WARNING("Deki3D: cannot read '%s'", absolutePath.c_str());
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();

    // The model's material library and its texture are named relative to the
    // model itself, and decoding an image is the editor's job, so both are
    // supplied here rather than reached for inside the parser.
    const std::string baseDirectory = fs::path(absolutePath).parent_path().string();
    auto decodeImage = [](const std::string& imagePath, int& width, int& height,
                          std::vector<uint8_t>& rgba) {
        DecodedImage decoded;
        if (!DecodeImageFile(imagePath, decoded))
            return false;
        width = decoded.width;
        height = decoded.height;
        rgba = std::move(decoded.rgba);
        return true;
    };
    auto chooseFormat = [&](bool hasAlpha) { return ChooseTexelFormat(absolutePath, target, hasAlpha); };

    std::vector<uint8_t> blob;
    std::string error;
    if (!Deki3D::CompileObjToMesh(text.str(), baseDirectory, decodeImage, blob, error, chooseFormat))
    {
        DEKI_LOG_WARNING("Deki3D: '%s' did not compile: %s", absolutePath.c_str(), error.c_str());
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        DEKI_LOG_WARNING("Deki3D: cannot write the compiled mesh to '%s'", outPath.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    DEKI_LOG_EDITOR("Deki3D: compiled '%s' to %zu bytes",
                    fs::path(absolutePath).filename().string().c_str(), blob.size());
    return true;
}

/// Compile one .obj into the cache, stored for the editor's target. Failure
/// leaves no cache file, so the component's asset stays unresolved and the
/// pass draws nothing rather than drawing something wrong.
void HandleObjSync(const std::string& absolutePath, const std::string& guid,
                   const std::string& projectPath)
{
    const AssetPipeline* pipeline = AssetPipeline::Instance();
    const AssetExportTarget target = pipeline ? pipeline->GetEditorTarget() : AssetExportTarget{};
    const fs::path cachePath = fs::path(GetCacheDirectory(projectPath)) / guid;
    if (!CompileObjFile(absolutePath, target, cachePath.string()))
        return;

    // The compiled blob lives in the cache under the source's own GUID, so
    // point the asset manager at it. Without this the GUID resolves to the
    // .obj text, and MeshAsset rejects that as not being a mesh.
    if (auto* assets = Deki::AssetManager::Get())
        assets->RegisterGuid(guid, guid);
}

/// Re-point every already-compiled model at its cache entry. The sync handler
/// above only runs when a source changes, so on a project that opens with its
/// cache already warm nothing would register the GUIDs otherwise.
void RegisterCompiledMeshes(AssetPipeline* pipeline, const std::string& projectPath)
{
    auto* assets = Deki::AssetManager::Get();
    if (!pipeline || !assets)
        return;

    const fs::path cacheDir(GetCacheDirectory(projectPath));
    for (const auto& entry : pipeline->GetAllAssets())
    {
        const std::string& relativePath = entry.first;
        if (relativePath.size() < 4 ||
            relativePath.compare(relativePath.size() - 4, 4, ".obj") != 0)
            continue;

        const std::string& guid = entry.second.guid;
        std::error_code ec;
        if (!guid.empty() && fs::exists(cacheDir / guid, ec))
            assets->RegisterGuid(guid, guid);
    }
}

// A static initialiser rather than work in the package entry point, so the
// vtable of the AssetTypeEditor above stays stable across package rebuilds
// (see the note in EditorExtension.h).
struct Deki3DAssetRegistrar
{
    Deki3DAssetRegistrar()
    {
        AssetTypeRegistry::Instance().RegisterCategory(".obj", AssetCategory::Data);

        AssetPipeline::OnStarted([](AssetPipeline* pipeline) {
            pipeline->RegisterSyncHandler(".obj", HandleObjSync);
            pipeline->RegisterExportEncoder(".obj", [](const AssetExportContext& ctx) {
                return CompileObjFile(ctx.absolutePath, ctx.target, ctx.outPath);
            });
        });

        AssetPipeline::OnImportComplete([](AssetPipeline* pipeline) {
            RegisterCompiledMeshes(pipeline, pipeline->GetProjectPath());
        });
    }
};
static Deki3DAssetRegistrar s_deki3DAssetRegistrar;

}  // namespace
}  // namespace DekiEditor

#endif  // DEKI_EDITOR
