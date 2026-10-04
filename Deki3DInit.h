#pragma once

/**
 * @brief Bring the package up on a static (firmware or simulator) build.
 *
 * Called from the generated DekiInitPackageSystems(). It registers the
 * Mesh loader and keeps the render pass in the link; the static registrars in
 * MeshAssetLoader.cpp and Mesh3DPass.cpp do the registering in a DLL, but a
 * static link drops a file nothing references, and nothing referenced those,
 * so a device had no Mesh loader and no pass at all.
 *
 * Global scope, like every package's _InitSystem: the generated file that
 * calls it declares it without knowing the package's namespace.
 */
void Deki3DInitSystem();
void Deki3DShutdownSystem();

/// Defined in Mesh3DPass.cpp and called by Deki3DInitSystem, only so a static
/// link keeps that file (and the pass's registrar).
void Deki3DKeepMesh3DPass();
