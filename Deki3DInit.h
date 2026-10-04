#pragma once

/// Starts the package on a static (firmware or simulator) build. Called from
/// the generated DekiInitPackageSystems(). It registers the Mesh loader and
/// keeps the render pass in the link. In a DLL the static registrars in
/// MeshAssetLoader.cpp and Mesh3DPass.cpp do this, but a static link drops a
/// file nothing references, which would leave a device with no loader and no
/// pass.
///
/// Global scope, like every package's _InitSystem: the generated file that
/// calls it declares it without knowing the package's namespace.
void Deki3DInitSystem();
void Deki3DShutdownSystem();

/// Defined in Mesh3DPass.cpp and called by Deki3DInitSystem, only so a static
/// link keeps that file (and the pass's registrar).
void Deki3DKeepMesh3DPass();
