#pragma once

// The GPU meshes' shader use (editor only): see MeshShaderUse.cpp.

#ifdef DEKI_EDITOR

#include "deki-rendering/editor/ShaderUses.h"

namespace Deki3D
{

/// The "mesh" use, as registered with deki-rendering.
DekiRendering::ShaderUse MeshShaderUse();

}  // namespace Deki3D

#endif  // DEKI_EDITOR
