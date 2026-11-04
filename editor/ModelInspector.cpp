#ifdef DEKI_EDITOR

// The inspector of a model (.obj): what the editor's build of it holds, and
// how much of it each of the project's platforms keeps (Detail: the share of
// its triangles). Its textures are images, stored and drawn as each image's
// own settings say, in the image's inspector.
//
// It draws through EditorUI and the editor's target cards, never ImGui: a
// package DLL has no ImGui context of its own.

#include "ModelSettings.h"
#include "ObjImporter.h"
#include "../MeshAsset.h"

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorTheme.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/Localization.h>
#include <deki-editor/Paths.h>
#include <deki-editor/TargetCards.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/LogSystem.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Deki3D
{
namespace
{

using DekiEditor::Tr;
using DekiEditor::TrMark;


const float kDetails[] = { 1.0f, 0.75f, 0.5f, 0.25f, 0.1f, 0.05f, 0.02f, 0.01f };
const char* const kDetailLabels[] = { TrMark("All"), "75%", "50%", "25%", "10%", "5%", "2%", "1%" };
constexpr int kDetailCount = 8;

int DetailIndex(float detail)
{
    int best = 0;
    for (int i = 1; i < kDetailCount; ++i)
    {
        if (std::fabs(kDetails[i] - detail) < std::fabs(kDetails[best] - detail))
        {
            best = i;
        }
    }
    return best;
}


/// What the editor's build of the model holds, read from its cache file.
struct Built
{
    bool valid = false;
    MeshFileHeader header{};
    uint64_t bytes = 0;
};

Built ReadBuilt(const std::string& projectPath, const std::string& guid)
{
    Built out;
    const fs::path path = fs::path(DekiEditor::GetCacheDirectory(projectPath)) / guid;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return out;
    }
    in.read(reinterpret_cast<char*>(&out.header), kMeshFileHeaderV4Bytes);
    if (!in || std::memcmp(out.header.magic, "DMSH", 4) != 0)
    {
        return out;
    }
    out.header.flags = 0;
    if (out.header.version >= 5)
    {
        in.read(reinterpret_cast<char*>(&out.header.flags), sizeof(out.header.flags));
    }
    std::error_code ec;
    out.bytes = fs::file_size(path, ec);
    out.valid = static_cast<bool>(in);
    return out;
}

std::string Thousands(uint64_t n)
{
    std::string digits = std::to_string(n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
    {
        digits.insert(static_cast<size_t>(i), ",");
    }
    return digits;
}

class ModelInspector : public DekiEditor::FileInspector
{
public:
    const char** GetExtensions() const override
    {
        static const char* s_Extensions[] = { ".obj" };
        return s_Extensions;
    }
    int GetExtensionCount() const override { return 1; }

    void OnInspectorGUI(const std::string& assetPath, const std::string& assetGuid) override
    {
        auto* pipeline = DekiEditor::AssetPipeline::Instance();
        if (!pipeline)
        {
            return;
        }
        const std::string projectPath = pipeline->GetProjectPath();
        const std::string absolutePath = (fs::path(projectPath) / assetPath).string();
        if (absolutePath != m_Path)
        {
            m_Path = absolutePath;
            m_Settings = ReadModelSettings(absolutePath);
            m_Open.clear();
        }
        DrawBuilt(projectPath, assetGuid);
        DrawTargets(projectPath, assetPath);
    }

private:
    void DrawBuilt(const std::string& projectPath, const std::string& guid)
    {
        auto& ui = DekiEditor::EditorUI::Get();
        DekiEditor::FullBleedSeparator();
        if (!DekiEditor::SchematicSectionBegin("Model"))
        {
            return;
        }
        const Built built = ReadBuilt(projectPath, guid);
        if (!built.valid)
        {
            ui.TextWrapped(Tr("Not built for the editor yet, or it did not compile: the Console says why."));
            DekiEditor::SchematicSectionEnd();
            return;
        }
        const MeshFileHeader& h = built.header;
        const bool wide = (h.flags & MeshFileIndex32) != 0;
        ui.PropertyRow(Tr("Triangles"));
        ui.ValueText(Thousands(h.indexCount / 3).c_str());
        ui.PropertyRow(Tr("Vertices"));
        ui.ValueText(Thousands(h.vertexCount).c_str());
        if (ui.IsItemHovered())
        {
            ui.SetTooltip(wide ? Tr("Over 65535, so its indices are 32-bit")
                               : Tr("Up to 65535, so its indices are 16-bit"));
        }
        ui.PropertyRow(Tr("Materials"));
        ui.ValueText(std::to_string(h.materialCount).c_str());
        ui.PropertyRow(Tr("Textures"));
        ui.ValueText(std::to_string(h.textureCount).c_str());
        if (ui.IsItemHovered())
        {
            ui.SetTooltip(Tr("Each is an image of the project, stored and drawn as that image's own settings say: "
                             "its Format, Max Size and Filter, per platform, in the image's inspector."));
        }
        char size[32];
        std::snprintf(size, sizeof(size), "%.1f MB", static_cast<double>(built.bytes) / (1024.0 * 1024.0));
        ui.PropertyRow(Tr("Size"));
        ui.ValueText(size);
        if (ui.IsItemHovered())
        {
            ui.SetTooltip(Tr("The editor's build, for the active platform. Each platform's build follows its card "
                             "below."));
        }
        DekiEditor::SchematicSectionEnd();
    }

    void Save(const std::string& assetPath)
    {
        auto* pipeline = DekiEditor::AssetPipeline::Instance();
        if (!WriteModelSettings(m_Path, m_Settings))
        {
            DEKI_LOG_ERROR("Deki3D: cannot write %s.data", m_Path.c_str());
            return;
        }
        // Builds it again for the editor, with the new settings.
        if (pipeline)
        {
            pipeline->RefreshAsset(assetPath);
        }
    }

    void DrawTargets(const std::string& projectPath, const std::string& assetPath)
    {
        if (!DekiEditor::TargetCardsBegin())
        {
            return;
        }
        auto& ui = DekiEditor::EditorUI::Get();
        const int detailIndex = DetailIndex(m_Settings.detail);

        {
            DekiEditor::TargetCardInfo info;
            info.id = "##default";
            info.name = Tr("Default");
            info.note = Tr("all targets");
            info.nameTip = Tr("What every target uses, unless its own card overrides it");
            info.summary = std::string(Tr("Detail")) + " " + Tr(kDetailLabels[detailIndex]);
            DekiEditor::TargetCard(info, m_Open["##default"],
                                   [&](float labelX, float valueX, float valueW)
                                   {
                                       int d = detailIndex;
                                       DekiEditor::TargetCardRow(TrMark("Detail"), labelX, valueX);
                                       ui.SetNextItemWidth(valueW);
                                       if (ui.Combo("##Detail", &d, kDetailLabels, kDetailCount))
                                       {
                                           m_Settings.detail = kDetails[d];
                                           Save(assetPath);
                                       }
                                       if (ui.IsItemHovered())
                                       {
                                           ui.SetTooltip(Tr("The share of its triangles kept. Seams, open edges and "
                                                            "material borders stay, so it may keep more; the build "
                                                            "says how many."));
                                       }
                                   });
        }

        char buf[96];
        for (const DekiEditor::PlatformConfig* platform : DekiEditor::ProjectTargets(projectPath))
        {
            const std::string& id = platform->id;
            const std::string name = platform->displayName.empty() ? id : platform->displayName;
            ModelTargetSettings modelTarget;
            if (auto it = m_Settings.targets.find(id); it != m_Settings.targets.end())
            {
                modelTarget = it->second;
            }
            // -1 is Default.
            int td = modelTarget.detail < 0.0f ? -1 : DetailIndex(modelTarget.detail);
            const char* shownDetail = kDetailLabels[td >= 0 ? td : detailIndex];
            const bool overridden = td >= 0;

            DekiEditor::TargetCardInfo info;
            info.id = id.c_str();
            info.overridden = overridden;
            info.name = name;
            info.nameTip = overridden ? Tr("Overrides Default") : nullptr;
            info.summary = std::string(Tr("Detail")) + " " + Tr(shownDetail);
            auto open = m_Open.find(id);
            if (open == m_Open.end())
            {
                open = m_Open.emplace(id, overridden).first;
            }
            DekiEditor::TargetCard(info, open->second,
                                   [&](float labelX, float valueX, float valueW)
                                   {
                                       DekiEditor::TargetCardRow(TrMark("Detail"), labelX, valueX);
                                       std::snprintf(buf, sizeof(buf), Tr("Default (%s)"),
                                                     Tr(kDetailLabels[detailIndex]));
                                       if (DekiEditor::TargetOverrideCombo(
                                               "##TargetDetail", valueW, Tr(shownDetail), td < 0, buf,
                                               [](int i) { return kDetailLabels[i]; }, kDetailCount, 0, td))
                                       {
                                           if (td < 0)
                                           {
                                               m_Settings.targets.erase(id);
                                           }
                                           else
                                           {
                                               m_Settings.targets[id].detail = kDetails[td];
                                           }
                                           Save(assetPath);
                                       }
                                   });
        }
        DekiEditor::TargetCardsEnd();
    }

    std::string m_Path;
    ModelSettings m_Settings;
    std::map<std::string, bool> m_Open;
};

}  // namespace

void RegisterModelInspector()
{
    static ModelInspector s_Inspector;
    DekiEditor::FileInspectorRegistry::Instance().Register(&s_Inspector);
}

}  // namespace Deki3D

#endif  // DEKI_EDITOR
