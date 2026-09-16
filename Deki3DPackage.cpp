/**
 * @file Deki3DPackage.cpp
 * @brief Package entry point for deki-3d.
 *
 * Exports the standard Deki plugin interface so the editor can load
 * deki-3d.dll and register its components. For a linked DLL,
 * Deki3D_EnsureRegistered() is what triggers the static initialisers.
 */

#include "Deki3DPackage.h"

#include <deki/interop/Plugin.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/ComponentRegistry.h>

extern void Deki3D_RegisterComponents();
extern int Deki3D_GetAutoComponentCount();
extern const Deki::ComponentMeta* Deki3D_GetAutoComponentMeta(int index);

namespace Deki3D
{

#ifdef DEKI_EDITOR

// Auto-generated registration helpers.

static bool s_3DRegistered = false;


// The exports below are C symbols at global scope; the package's own
// registration helpers and statics live in its namespace.
using namespace Deki3D;

extern "C" {

DEKI_3D_API int Deki3D_EnsureRegistered(void)
{
    if (s_3DRegistered)
        return ::Deki3D_GetAutoComponentCount();
    s_3DRegistered = true;
    ::Deki3D_RegisterComponents();
    return ::Deki3D_GetAutoComponentCount();
}

DEKI_PLUGIN_API const char* DekiPlugin_GetName(void)
{
    return "DekiRendering::Deki 3D Package";
}

DEKI_PLUGIN_API const char* DekiPlugin_GetVersion(void)
{
#ifdef DEKI_PACKAGE_VERSION
    return DEKI_PACKAGE_VERSION;
#else
    return "0.0.0-dev";
#endif
}

DEKI_PLUGIN_API int DekiPlugin_Init(void)
{
    // The render pass registers itself from Mesh3DPass.cpp, so there is
    // nothing to start here.
    return 0;
}

DEKI_PLUGIN_API void DekiPlugin_Shutdown(void)
{
    s_3DRegistered = false;
}

DEKI_PLUGIN_API int DekiPlugin_GetComponentCount(void)
{
    return ::Deki3D_GetAutoComponentCount();
}

DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPlugin_GetComponentMeta(int index)
{
    return ::Deki3D_GetAutoComponentMeta(index);
}

DEKI_PLUGIN_API void DekiPlugin_RegisterComponents(void)
{
    Deki3D_EnsureRegistered();
}

// deki-3d draws no editor UI of its own, so it links no ImGui and shares no
// ImGui context. Its component inspectors are drawn by the editor via
// reflection.

DEKI_3D_API const char* Deki3D_GetName(void)
{
    return "3D";
}

}  // extern "C"

#endif  // DEKI_EDITOR
}  // namespace Deki3D

