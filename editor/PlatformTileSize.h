#pragma once

// The tile size a platform's build gives a scene that leaves it to the
// platform (Mesh3DTiles::Platform): what kPlatformTileSize comes out as when
// that platform compiles Mesh3DSettings.h, read from its platform file
// instead, so the editor can say it for every target. Editor only.

#include "../Mesh3DSettings.h"

#include <deki-editor/build/PlatformConfig.h>

#include <cstdlib>
#include <string>

namespace Deki3D
{

/// The define that sets a platform's tile size, as its platform file names it.
inline constexpr const char* kTileSizeDefine = "DEKI_3D_TILE_SIZE";

/// What `platform` draws a Platform scene's tiles at: its DEKI_3D_TILE_SIZE,
/// or 32 when it names none. 0 for a value that is not a whole number from 8
/// to 512, which its build refuses to compile.
inline int32_t PlatformTileSize(const DekiEditor::PlatformConfig& platform)
{
    const std::string prefix = std::string(kTileSizeDefine) + "=";
    for (const std::string& define : platform.defines)
    {
        if (define == kTileSizeDefine)
        {
            return 0;  // defined as 1
        }
        if (define.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        const char* text = define.c_str() + prefix.size();
        char* end = nullptr;
        const long value = std::strtol(text, &end, 10);
        if (end == text || *end != '\0' || value < 8 || value > 512)
        {
            return 0;
        }
        return static_cast<int32_t>(value);
    }
    return kUnnamedPlatformTileSize;
}

}  // namespace Deki3D
