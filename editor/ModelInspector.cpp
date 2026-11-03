#ifdef DEKI_EDITOR

// The inspector of a model (.obj): what the editor's build of it holds, and
// what each of the project's platforms stores. Default sets what every
// platform gets; a platform's card may differ:
//
//   Format    its textures' format (Automatic: the platform's screen's)
//   Max Size  its textures' largest side (Automatic: none where the platform
//             has a GPU, 256 on a board)
//   Filter    Point, or Smooth: blended and mipmapped, on a GPU
//   Detail    the share of its triangles kept
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

const char* const kFormats[] = { TrMark("Automatic"), "RGB888", "RGBA8888", "RGB565", "RGB565A8", "ALPHA8" };
constexpr int kFormatCount = 6;

// Default's labels; a target's list says None where Default says Automatic,
// since a target's 0 is no cap.
const char* const kMaxSizes[] = { TrMark("Automatic"), "32", "64", "128", "256", "512", "1024", "2048", "4096", "8192" };
const char* const kTargetMaxSizes[] = { TrMark("None"), "32", "64", "128", "256", "512", "1024", "2048", "4096", "8192" };
static_assert(sizeof(kMaxSizes) / sizeof(kMaxSizes[0]) == DekiEditor::kTextureMaxSizeCount, "one label per size");

const char* const kFilters[] = { TrMark("Point"), TrMark("Smooth") };
constexpr int kFilterCount = 2;

const float kDetails[] = { 1.0f, 0.75f, 0.5f, 0.25f, 0.1f, 0.05f, 0.02f, 0.01f };
const char* const kDetailLabels[] = { TrMark("All"), "75%", "50%", "25%", "10%", "5%", "2%", "1%" };
constexpr int kDetailCount = 8;

int FormatIndex(const std::string& name)
{
    for (int i = 1; i < kFormatCount; ++i)
    {
        if (name == kFormats[i])
        {
            return i;
        }
    }
    return 0;
}

int MaxSizeIndex(int maxSize)
{
    int best = 0;
    for (int i = 0; i < DekiEditor::kTextureMaxSizeCount; ++i)
    {
        if (DekiEditor::kTextureMaxSizes[i] <= maxSize)
        {
            best = i;
        }
    }
    return best;
}

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

bool HasGpu(const DekiEditor::PlatformConfig& platform)
{
    return platform.Capabilities().count("opengl") != 0;
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
        DekiEditor::TextureSettings& tex = m_Settings.texture;
        const int formatIndex = FormatIndex(tex.format);
        const int maxIndex = MaxSizeIndex(tex.maxSize);
        const int filterIndex = m_Settings.filter == "Smooth" ? 1 : 0;
        const int detailIndex = DetailIndex(m_Settings.detail);
        auto summary = [&](const char* format, const char* maxSize, const char* filter, const char* detail)
        {
            return std::string(Tr(format)) + "  \xC2\xB7  " + Tr("Max Size") + " " + Tr(maxSize) + "  \xC2\xB7  " +
                   Tr(filter) + "  \xC2\xB7  " + Tr(detail);
        };

        {
            DekiEditor::TargetCardInfo info;
            info.id = "##default";
            info.name = Tr("Default");
            info.note = Tr("all targets");
            info.nameTip = Tr("What every target uses, unless its own card overrides it");
            info.summary =
                summary(kFormats[formatIndex], kMaxSizes[maxIndex], kFilters[filterIndex], kDetailLabels[detailIndex]);
            DekiEditor::TargetCard(info, m_Open["##default"],
                                   [&](float labelX, float valueX, float valueW)
                                   {
                                       int f = formatIndex, m = maxIndex, fi = filterIndex, d = detailIndex;
                                       DekiEditor::TargetCardRow(TrMark("Format"), labelX, valueX);
                                       ui.SetNextItemWidth(valueW);
                                       if (ui.Combo("##Format", &f, kFormats, kFormatCount))
                                       {
                                           tex.format = f == 0 ? std::string() : kFormats[f];
                                           Save(assetPath);
                                       }
                                       DekiEditor::TargetCardRow(TrMark("Max Size"), labelX, valueX);
                                       ui.SetNextItemWidth(valueW);
                                       if (ui.Combo("##MaxSize", &m, kMaxSizes, DekiEditor::kTextureMaxSizeCount))
                                       {
                                           tex.maxSize = DekiEditor::kTextureMaxSizes[m];
                                           Save(assetPath);
                                       }
                                       if (ui.IsItemHovered())
                                       {
                                           ui.SetTooltip(Tr("Automatic: no limit where the platform has a GPU, 256 "
                                                            "on a board"));
                                       }
                                       DekiEditor::TargetCardRow(TrMark("Filter"), labelX, valueX);
                                       ui.SetNextItemWidth(valueW);
                                       if (ui.Combo("##Filter", &fi, kFilters, kFilterCount))
                                       {
                                           m_Settings.filter = fi == 1 ? "Smooth" : "";
                                           Save(assetPath);
                                       }
                                       if (ui.IsItemHovered())
                                       {
                                           ui.SetTooltip(Tr("Smooth blends between texels and uses smaller copies "
                                                            "far away, on a GPU. A board draws Point."));
                                       }
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
            const bool gpu = HasGpu(*platform);
            DekiEditor::TextureTargetSettings texTarget;
            if (auto it = tex.targets.find(id); it != tex.targets.end())
            {
                texTarget = it->second;
            }
            ModelTargetSettings modelTarget;
            if (auto it = m_Settings.targets.find(id); it != m_Settings.targets.end())
            {
                modelTarget = it->second;
            }

            // Index -1 (or 0 for Format) is Default; the shown value is what
            // the target gets.
            int tf = texTarget.format.empty() ? 0 : FormatIndex(texTarget.format);
            int tm = texTarget.maxSize < 0 ? -1 : MaxSizeIndex(texTarget.maxSize);
            int tfi = modelTarget.filter.empty() ? -1 : (modelTarget.filter == "Smooth" ? 1 : 0);
            int td = modelTarget.detail < 0.0f ? -1 : DetailIndex(modelTarget.detail);
            const char* defaultMax = maxIndex > 0 ? kMaxSizes[maxIndex] : (gpu ? TrMark("None") : "256");
            const char* shownFormat = kFormats[tf > 0 ? tf : formatIndex];
            const char* shownMax = tm >= 0 ? kTargetMaxSizes[tm] : defaultMax;
            const char* shownFilter = kFilters[tfi >= 0 ? tfi : filterIndex];
            const char* shownDetail = kDetailLabels[td >= 0 ? td : detailIndex];
            const bool overridden = tf > 0 || tm >= 0 || tfi >= 0 || td >= 0;

            auto writeTarget = [&]()
            {
                DekiEditor::TextureTargetSettings t;
                t.format = tf > 0 ? kFormats[tf] : "";
                t.maxSize = tm >= 0 ? DekiEditor::kTextureMaxSizes[tm] : -1;
                if (t.Empty())
                {
                    tex.targets.erase(id);
                }
                else
                {
                    tex.targets[id] = t;
                }
                ModelTargetSettings mt;
                mt.filter = tfi < 0 ? "" : (tfi == 1 ? "Smooth" : "Point");
                mt.detail = td < 0 ? -1.0f : kDetails[td];
                if (mt.Empty())
                {
                    m_Settings.targets.erase(id);
                }
                else
                {
                    m_Settings.targets[id] = mt;
                }
                Save(assetPath);
            };

            DekiEditor::TargetCardInfo info;
            info.id = id.c_str();
            info.overridden = overridden;
            info.name = name;
            info.note = gpu ? Tr("GPU") : Tr("no GPU");
            info.noteTip = gpu ? Tr("Draws meshes through OpenGL: Smooth filtering and shadows")
                               : Tr("Draws meshes in software: textures unfiltered, no shadows");
            info.nameTip = overridden ? Tr("Overrides Default") : nullptr;
            info.summary = summary(shownFormat, shownMax, shownFilter, shownDetail);
            auto open = m_Open.find(id);
            if (open == m_Open.end())
            {
                open = m_Open.emplace(id, overridden).first;
            }
            DekiEditor::TargetCard(
                info, open->second,
                [&](float labelX, float valueX, float valueW)
                {
                    DekiEditor::TargetCardRow(TrMark("Format"), labelX, valueX);
                    std::snprintf(buf, sizeof(buf), Tr("Default (%s)"), Tr(kFormats[formatIndex]));
                    if (DekiEditor::TargetOverrideCombo("##TargetFormat", valueW, Tr(shownFormat), tf == 0, buf,
                                                        [](int i) { return kFormats[i]; }, kFormatCount, 1, tf))
                    {
                        writeTarget();
                    }
                    DekiEditor::TargetCardRow(TrMark("Max Size"), labelX, valueX);
                    std::snprintf(buf, sizeof(buf), Tr("Default (%s)"), Tr(defaultMax));
                    if (DekiEditor::TargetOverrideCombo("##TargetMax", valueW, Tr(shownMax), tm < 0, buf,
                                                        [](int i) { return kTargetMaxSizes[i]; },
                                                        DekiEditor::kTextureMaxSizeCount, 0, tm))
                    {
                        writeTarget();
                    }
                    DekiEditor::TargetCardRow(TrMark("Filter"), labelX, valueX);
                    std::snprintf(buf, sizeof(buf), Tr("Default (%s)"), Tr(kFilters[filterIndex]));
                    if (DekiEditor::TargetOverrideCombo("##TargetFilter", valueW, Tr(shownFilter), tfi < 0, buf,
                                                        [](int i) { return kFilters[i]; }, kFilterCount, 0, tfi))
                    {
                        writeTarget();
                    }
                    if (!gpu && (tfi >= 0 ? tfi : filterIndex) == 1)
                    {
                        DekiEditor::TargetCardRow("", labelX, valueX);
                        ui.TextDisabled(Tr("No GPU here: drawn Point"));
                    }
                    DekiEditor::TargetCardRow(TrMark("Detail"), labelX, valueX);
                    std::snprintf(buf, sizeof(buf), Tr("Default (%s)"), Tr(kDetailLabels[detailIndex]));
                    if (DekiEditor::TargetOverrideCombo("##TargetDetail", valueW, Tr(shownDetail), td < 0, buf,
                                                        [](int i) { return kDetailLabels[i]; }, kDetailCount, 0, td))
                    {
                        writeTarget();
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
