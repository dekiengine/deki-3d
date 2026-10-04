// Registers a loader for compiled meshes with the engine's AssetManager.
//
// Kept apart from MeshAsset.cpp: the parser there is pure and its
// malformed-input tests run in a plain test binary, while this file is glue
// that needs a running engine.

#include "MeshAsset.h"
#include "Deki3DInit.h"

#include <deki/assets/AssetManager.h>

#include <vector>

namespace Deki3D
{

void Deki3DRegisterMeshLoader()
{
    static bool s_Registered = false;
    if (s_Registered)
    {
        return;
    }
    s_Registered = true;

    // LoadByGuidAndType uses the path loader for a cacheable type like this
    // one; its memory branch is for types reloaded fresh each time. The memory
    // loader is still registered because packed assets on a device load
    // through memory.
    auto loader = [](const char* path) -> void*
    {
        // The whole file, held briefly until it is parsed into the asset's
        // buffers. External like them: the internal heap of a board without
        // PSRAM cannot fit it, and the board would reboot.
        Deki::Buffer<uint8_t> bytes;
        if (!Deki::AssetManager::ReadWholeFile(path, bytes, Deki::Memory::External))
        {
            return nullptr;
        }
        auto* mesh = new MeshAsset();
        if (mesh->LoadFromMemory(bytes.Data(), bytes.Count()))
        {
            return mesh;
        }
        delete mesh;
        return nullptr;
    };
    auto memLoader = [](const uint8_t* data, size_t size) -> void*
    {
        auto* mesh = new MeshAsset();
        if (mesh->LoadFromMemory(data, size))
        {
            return mesh;
        }
        delete mesh;
        return nullptr;
    };
    auto unloader = [](void* asset) { delete static_cast<MeshAsset*>(asset); };

    Deki::AssetManager::RegisterLoader(MeshAsset::kAssetTypeName, loader, unloader, memLoader);
}

namespace
{
// A static registrar, not a call from the entry point: the device build has
// no DekiPluginInit. deki-2d's sprite and animation loaders do the same.
struct MeshLoaderRegistrar
{
    MeshLoaderRegistrar() { Deki3DRegisterMeshLoader(); }
};
static MeshLoaderRegistrar s_MeshLoaderRegistrar;
}  // namespace

}  // namespace Deki3D

void Deki3DInitSystem()
{
    Deki3D::Deki3DRegisterMeshLoader();
    Deki3DKeepMesh3DPass();
}

void Deki3DShutdownSystem()
{
}
