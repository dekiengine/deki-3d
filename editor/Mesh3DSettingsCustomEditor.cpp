#ifdef DEKI_EDITOR

// Mesh3DSettings in the editor: its inspector, which says what tile size a
// scene that leaves it to the platform gets, on the target the editor stores
// assets for and on each of the project's targets, since that is in each
// platform's file and not in the scene.

#include "PlatformTileSize.h"
#include "../Mesh3DSettings.h"

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorApplication.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/TargetCards.h>
#include <deki-editor/build/PlatformConfig.h>

#include <string>

// Editor extensions live in DekiEditor; the package's own types are in Deki3D.
using namespace Deki3D;

namespace DekiEditor
{
namespace
{

/// "64", or what is wrong with the platform's define.
std::string TileSizeText(int32_t size)
{
    return size > 0 ? std::to_string(size) : std::string(kTileSizeDefine) + " is not 8 to 512";
}

std::string NameOf(const PlatformConfig& platform)
{
    return platform.displayName.empty() ? platform.id : platform.displayName;
}

}  // namespace

class Mesh3DSettingsCustomEditor : public CustomEditor
{
public:
    const char* GetComponentName() const override { return "Mesh3DSettings"; }

    bool WantsInspectorOverride(Deki::Component* comp) override { return comp != nullptr; }

    void OnInspectorGUI(Deki::Component* comp) override
    {
        auto& ui = EditorUI::Get();
        auto* settings = static_cast<Mesh3DSettings*>(comp);
        ui.PropertyField("renderer");
        ui.PropertyField("composeOnGpu");
        ui.PropertyField("tiles");
        if (settings->tiles == Mesh3DTiles::Fixed)
        {
            ui.PropertyField("tileSize");
        }
        else
        {
            DrawPlatformTileSize(ui);
        }
        ui.PropertyField("perspectiveCorrect");
        ui.PropertyField("vertexSnap");
        ui.PropertyField("fillThreads");
    }

private:
    // The size on the editor's target, or the editor's own preview's when it
    // has none, and under it every target's.
    static void DrawPlatformTileSize(EditorUI& ui)
    {
        AssetPipeline* pipeline = AssetPipeline::Instance();
        const std::string targetId = pipeline ? pipeline->GetEditorTarget().platformId : std::string();
        const PlatformConfig* target = targetId.empty() ? nullptr : PlatformRegistry::Instance().GetPlatform(targetId);
        ui.PropertyRow("Tile Size");
        ui.ValueText((target ? TileSizeText(PlatformTileSize(*target)) + " on " + NameOf(*target)
                             : std::to_string(kPlatformTileSize) + " in the editor")
                         .c_str());

        std::string each;
        for (const PlatformConfig* platform : ProjectTargets(EditorApplication::Get().GetProjectPath()))
        {
            each += (each.empty() ? "" : ", ") + NameOf(*platform) + " " + TileSizeText(PlatformTileSize(*platform));
        }
        if (!each.empty())
        {
            ui.Hint(("Each platform's own (its " + std::string(kTileSizeDefine) + " define, or 32): " + each +
                     "; the editor's preview " + std::to_string(kPlatformTileSize) + ".")
                        .c_str());
        }
    }
};

REGISTER_EDITOR(Mesh3DSettingsCustomEditor)

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
