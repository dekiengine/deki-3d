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
#include <deki/LogSystem.h>
#include <deki/assets/AssetManager.h>

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

/// Compile one .obj into the cache. Failure is logged and leaves no cache
/// file, so the component's asset stays unresolved and the pass draws nothing
/// rather than drawing something wrong.
void HandleObjSync(const std::string& absolutePath, const std::string& guid,
                   const std::string& projectPath)
{
    std::ifstream in(absolutePath, std::ios::binary);
    if (!in)
    {
        DEKI_LOG_WARNING("Deki3D: cannot read '%s'", absolutePath.c_str());
        return;
    }
    std::ostringstream text;
    text << in.rdbuf();

    std::vector<uint8_t> blob;
    std::string error;
    if (!Deki3D::CompileObjToMesh(text.str(), blob, error))
    {
        DEKI_LOG_WARNING("Deki3D: '%s' did not compile: %s", absolutePath.c_str(), error.c_str());
        return;
    }

    const fs::path cachePath = fs::path(GetCacheDirectory(projectPath)) / guid;
    std::error_code ec;
    fs::create_directories(cachePath.parent_path(), ec);

    std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        DEKI_LOG_WARNING("Deki3D: cannot write the compiled mesh to '%s'",
                         cachePath.string().c_str());
        return;
    }
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    out.close();

    // The compiled blob lives in the cache under the source's own GUID, so
    // point the asset manager at it. Without this the GUID resolves to the
    // .obj text, and MeshAsset rejects that as not being a mesh.
    if (auto* assets = Deki::AssetManager::Get())
        assets->RegisterGuid(guid, guid);

    DEKI_LOG_EDITOR("Deki3D: compiled '%s' to %zu bytes",
                    fs::path(absolutePath).filename().string().c_str(), blob.size());
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
