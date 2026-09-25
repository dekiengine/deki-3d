#pragma once

/**
 * @brief Bring the package up on a static (firmware or simulator) build.
 *
 * Called from the generated deki_init_package_systems(). It registers the
 * Mesh loader; the static registrar in MeshAssetLoader.cpp does that in a
 * DLL, but a static link drops a file nothing references, and nothing else
 * references that one, so a device had no Mesh loader at all.
 *
 * Global scope, like every package's _InitSystem: the generated file that
 * calls it declares it without knowing the package's namespace.
 */
void Deki3D_InitSystem();
void Deki3D_ShutdownSystem();
