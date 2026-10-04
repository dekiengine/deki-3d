/**
 * @file Deki3DPackage.cpp
 * @brief Package entry point for deki-3d.
 *
 * Exports the standard Deki plugin interface so the editor can load
 * deki-3d.dll and register its components. For a linked DLL,
 * Deki3DEnsureRegistered() is what triggers the static initialisers.
 */

#include "Deki3DPackage.h"

#include <deki/interop/Plugin.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/ComponentRegistry.h>

extern void Deki3DRegisterComponents();
extern int Deki3DGetAutoComponentCount();
extern const Deki::ComponentMeta* Deki3DGetAutoComponentMeta(int index);

namespace Deki3D
{

#ifdef DEKI_EDITOR

// Auto-generated registration helpers.

static bool s_Deki3DRegistered = false;

// The exports below are C symbols at global scope; the package's own
// registration helpers and statics live in its namespace.
using namespace Deki3D;

extern "C"
{
    DEKI_3D_API int Deki3DEnsureRegistered(void)
    {
        if (s_Deki3DRegistered)
        {
            return ::Deki3DGetAutoComponentCount();
        }
        s_Deki3DRegistered = true;
        ::Deki3DRegisterComponents();
        return ::Deki3DGetAutoComponentCount();
    }

    DEKI_PLUGIN_API const char* DekiPluginGetName(void)
    {
        return "Deki 3D Package";
    }

    DEKI_PLUGIN_API const char* DekiPluginGetVersion(void)
    {
#ifdef DEKI_PACKAGE_VERSION
        return DEKI_PACKAGE_VERSION;
#else
        return "0.0.0-dev";
#endif
    }

    DEKI_PLUGIN_API int DekiPluginInit(void)
    {
        // The render pass registers itself from Mesh3DPass.cpp, so there is
        // nothing to start here.
        return 0;
    }

    DEKI_PLUGIN_API void DekiPluginShutdown(void)
    {
        s_Deki3DRegistered = false;
    }

    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return ::Deki3DGetAutoComponentCount();
    }

    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int index)
    {
        return ::Deki3DGetAutoComponentMeta(index);
    }

    DEKI_PLUGIN_API void DekiPluginRegisterComponents(void)
    {
        Deki3DEnsureRegistered();
    }

    // deki-3d draws no editor UI of its own, so it links no ImGui and shares no
    // ImGui context. Its component inspectors are drawn by the editor via
    // reflection.

    DEKI_3D_API const char* Deki3DGetName(void)
    {
        return "3D";
    }

}  // extern "C"

#endif  // DEKI_EDITOR
}  // namespace Deki3D
