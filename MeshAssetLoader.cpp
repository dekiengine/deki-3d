/**
 * @file MeshAssetLoader.cpp
 * @brief Teaches the engine's AssetManager to load a compiled mesh.
 *
 * Separate from MeshAsset.cpp on purpose: parsing a blob is pure and can be
 * exercised without an engine, a project or a DLL, while this file is glue
 * that only means anything inside a running engine. Keeping them apart is
 * what lets the parser's malformed-input cases run in a plain test binary.
 */

#include "MeshAsset.h"
#include "Deki3DInit.h"

#include <deki/assets/AssetManager.h>

#include <vector>

namespace Deki3D
{

void Deki3D_RegisterMeshLoader()
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    // The path loader is the one that actually runs. A cacheable type never
    // reaches the memory loader through LoadByGuidAndType: that branch is for
    // types the manager reloads fresh each time. Both are registered because
    // the packed-asset path on device does go through memory.
    auto loader = [](const char* path) -> void* {
        std::vector<uint8_t> bytes;
        if (!Deki::AssetManager::ReadWholeFile(path, bytes) || bytes.empty())
            return nullptr;
        auto* mesh = new MeshAsset();
        if (mesh->LoadFromMemory(bytes.data(), bytes.size()))
            return mesh;
        delete mesh;
        return nullptr;
    };
    auto memLoader = [](const uint8_t* data, size_t size) -> void* {
        auto* mesh = new MeshAsset();
        if (mesh->LoadFromMemory(data, size))
            return mesh;
        delete mesh;
        return nullptr;
    };
    auto unloader = [](void* asset) { delete static_cast<MeshAsset*>(asset); };

    Deki::AssetManager::RegisterLoader(MeshAsset::AssetTypeName, loader, unloader, memLoader);
}

namespace
{
// A static registrar rather than a call from the entry point: the device
// build has no DekiPlugin_Init, and this is the pattern deki-2d's sprite and
// animation loaders already use.
struct MeshLoaderRegistrar
{
    MeshLoaderRegistrar() { Deki3D_RegisterMeshLoader(); }
};
static MeshLoaderRegistrar s_meshLoaderRegistrar;
}  // namespace

}  // namespace Deki3D

void Deki3D_InitSystem()
{
    Deki3D::Deki3D_RegisterMeshLoader();
}

void Deki3D_ShutdownSystem() {}
