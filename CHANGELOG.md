# Changelog

Notable changes to `deki-3d`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### The software rasteriser draws a model of any size, faster
- A frame of more triangles than `RasterConfig::batchTriangles` (8192) is
  drawn in batches against one depth buffer for the screen, so any mesh draws
  in fixed memory; a frame within one batch fills tile by tile as before. The
  triangles and tile lists are in PSRAM where the board has it
  (`RegionArray`): as std::vectors they took a board's internal heap and, with
  Sponza, aborted the T-Deck. With no memory for another batch the frame draws
  what it has and says so once. The build's memory check counts this working
  memory (`Raster3D::WorkingBytes`) beside each model on a board without
  OpenGL.
- A part of a mesh out of view is skipped (`Submesh3D` bounds, worked out when
  the mesh loads), and a triangle outside the view or facing away is dropped
  before it is lit.
- Each vertex is transformed and lit once per part, not once per triangle
  using it (a 128-vertex cache).
- Each row of a triangle is filled from where its edges cross the row, not by
  testing every pixel of its box; setup and the edge walk are 32-bit for a
  triangle near the screen. The same pixels as before.
- `fillThreads` 2 fills on the second core of an ESP32-S3: the fill worker is
  pinned to the core the engine is not on (it ran on the same one). deki-3d
  now needs ESP-IDF's `pthread` component.
- `RasterStats` counts batches, triangles dropped, parts skipped, and the time
  spent on geometry and on filling.
- Sponza (Detail 10%, 38k triangles, one directional and one ambient light) on
  a LilyGO T-Deck: 1.7 frames a second, from a reboot loop; at 0.6 before the
  last three changes.

### Detail collapses texture seams
- A seam's copies of a vertex collapse as one, each onto the copy of the
  target on its own side, so the seam stays shut and each side keeps its
  texture; only a seam's corner, an open edge and a material border stay
  where they are. Sponza at 10% keeps 38511 triangles where it kept 82291.
  `kDekiCacheVersion` 8 compiles every model again.

### Models at any size, built for each platform
- A mesh of any size compiles: 16-bit indices up to 65535 vertices, 32-bit
  past that (`Mesh3D::indexBytes`, `Mesh3D::IndexAt`). Both renderers draw
  either. Mesh files are version 5 (the header ends in `flags`); version 4
  files still load.
- Each platform's build keeps what it can use. A model's own setting is
  Detail, per platform: the share of triangles kept (`editor/MeshSimplify.h`:
  edge collapse by quadric error, never moving a seam, an open edge or a
  material border, never turning a triangle over).
- A model's textures follow their images' own settings for the platform:
  Format, Max Size and Filter (Point, Bilinear, Trilinear;
  `Texture3D::filter`, from the engine's `TextureFilter`), set in the image's
  inspector, several at once. There is no cap of the model's own: a board's
  build says when a model does not fit. On the GPU Bilinear is linear, and
  Trilinear linear between mipmaps; the software rasteriser draws Point.
  `MeshCompileOptions::textureFor` gives each texture's settings by its
  image's path.
- What a model is compiled from (`ObjSourceFiles`: its material library and
  images) is part of its cache stamp, so a change to one rebuilds it.
- The Model inspector (`.obj`): what the editor's build holds, and the
  Targets cards for Detail, Default and per platform. Settings in
  `editor/ModelSettings.h`; a model's `.data` from before, with texture
  settings of its own, loses them when saved.
- A board build stops when a model needs more external memory than the board
  has, naming its images' Max Size and its Detail, and warns when Detail
  stops above what it asks.
- A mesh loads from its file part by part, each straight into its own buffer
  (`MeshAsset::LoadFromFile`), so loading takes the mesh's size in memory. The
  loader read the whole file in first and then copied it, so a load held twice
  the mesh (8.6 MB for a 4.3 MB one), and a mesh the build check let through
  could still not load on the board.
- The importer reads `map_d` as a texture's cutout and paths written with
  backslashes. `CompileObjToMesh` takes a `MeshCompileOptions` (each
  texture's settings, detail, and a report of what it did) in place of its
  last two arguments.
- Breaking: `Mesh3D::indices` is a `const void*` read through `IndexAt`.
- Tests: `BigMeshTests.cpp`, `MeshSimplifyTests.cpp`, the GPU drawing past
  65535 vertices, bilinear and trilinear textures, and an off-centre shadow, which the
  centred cube test could not catch a mirrored lookup with. By hand:
  `DISABLED_CompileAModel` and `DISABLED_AModelUnderTheSun` take a real model
  (`DEKI_TEST_OBJ`).

### Lights and shadows
- Meshes are lit by the scene's `DekiRendering::Light` components: an
  ambient colour and up to 8 directional, point and spot lights, in colour,
  by distance, cone and facing, up to twice a colour (`LightAt`,
  `Light3D`, `RasterConfig::lights`). The software rasteriser lights a mesh
  by the 4 lights that reach it most and shades per channel.
- Shadows on the GPU, from up to two directional or spot lights a frame
  (`ShadowMap`, `Mesh3DBackend::DrawShadowCaster`), cast by every mesh with
  the new `MeshComponent::castShadows` (on by default). The software
  rasteriser draws without them and says so.
- A scene without a Light draws its lit meshes unlit and says so once per
  mesh; a lit mesh `LightsItself()`, so the 2D light goes under it.
- Breaking: `Mesh3DSettings` loses `lightYaw`, `lightPitch` and `ambient`; a
  scene saved with them opens with an Ambient and a Directional Light that
  light it as before. `RasterConfig::lightDirection` and `ambient` (a float)
  become `ambient` (a colour) and `lights`. `DrawMesh` takes the model
  matrix, `Identity` by default.
- Breaking, for shaders: `Fragment.light` is a `float3`, one light per
  channel; `Fragment.position` (world space) and `LightAt(position, normal)`
  are new. `uLightDir` is gone.
- `Ortho` in Math3D.h.

### Added
- `Mesh3DBackend`: what draws the pass's meshes. `Raster3D` is one;
  `GLMesh3DBackend` draws them on the GPU, through OpenGL ES 3.0 or desktop
  OpenGL 3.3, where the platform provides `opengl` (`DEKI_CAPABILITY_OPENGL`).
  A board without it compiles none of it. `RasterConfig` and `RasterStats` moved to its
  header (`Raster3D.h` still includes it).
- `Mesh3DSettings.renderer`: Auto (the GPU where the platform has OpenGL),
  Software or GPU.
- `MeshComponent.shader`, `shaderParams` and `shaderColor`, and the same on
  `Material3D`: a GPU shader for the mesh (deki-rendering's `ShaderAsset`),
  in HLSL with `Displace()` and `Shade()` hooks (`editor/MeshShaderUse.cpp`,
  which the built-in program is generated from too). The software rasteriser
  draws the shading model and warns once.
- `Mesh3DSettings.composeOnGpu` and `RasterConfig::composeOnGpu`: a GPU
  backend may draw straight into a frame the display builds on the GPU,
  leaving the CPU buffer with only the 2D drawn after the meshes. On by
  default; off for a scene that reads the whole frame back.
- `Mesh3D::revision` and `Texture3D::revision`, which change with the data so
  a GPU backend can keep its uploads. Mesh assets and the built-in primitives
  set them (`NextRevision()`).
- `Mesh3DBackend::WritesBuffer`: whether a batch reached the frame through
  the buffer. Where the 2D is drawn on the GPU too (deki-rendering's
  `QuadBlit::BlitSink`), `Mesh3DPass` tells it after a batch that did (the
  software rasteriser, a GPU backend reading back), so the meshes stay
  between the 2D drawn before and after them. The pass itself says it writes
  nothing (`RenderPass::WritesBuffer`).

### Fixed
- Tint colours were packed in the wrong byte order (`ToRGBA8888` where
  materials keep 0xAABBGGRR), so any tint but white came out with its
  channels swapped. With the default tint, opaque black, that turned every
  mesh red. The default is now white, as its tooltip always said; a scene
  that saved the old default now draws its meshes black, as it asks.
- Big-endian boards (`DEKI_RGB565_BIG_ENDIAN`): RGB565 into an RGB565
  framebuffer came out byte-swapped, while every other framebuffer format was
  right. Meshes now hold RGB565 and RGB565A8 textures in memory's byte order
  once loaded, as images do, and the rasteriser reads and writes 565 values
  in that order. Nothing changes on little-endian boards.

## 0.22.0

### Changed
- `minEngine` 0.22.0: built against engine 0.22.0. A build of this package
  from before does not load and is rebuilt.

## 0.19.0

### Changed
- `minEngine` 0.19.0: built against engine 0.19.0, whose component metadata records the package each class is in. A build of this package from before does not load and is rebuilt.

### Added
- Japanese, Simplified Chinese and Korean: `editor/lang/ja.json`, `zh.json`
  and `ko.json` translate the names, tooltips and enum values this
  package's components show in the Inspector.
- The package's editor text goes through the editor's translation (`Tr`), so
  it shows in the editor's language. Needs the editor release that adds
  `Localization.h`; raise `minEngine` to it when releasing.

## 0.18.0

### Changed
- `minEngine` 0.18.0. Reflection ABI 21: the package must be rebuilt.
- **Names follow the code style** (deki-engine/docs/codestyle): types, functions and enum values are PascalCase, constants kPascalCase, members m_PascalCase, locals and parameters camelCase. The code is formatted with clang-format 22.
- The functions the editor finds by name are PascalCase: Deki3DRegisterComponents, Deki3DGetAutoComponentCount, Deki3DEnsureRegistered and the rest. Built against engine ABI 21; a build of this package from before does not load and is rebuilt.
- Renamed: `MeshAttribute_Position` and the other mesh flags are `MeshAttributePosition`, ...; `MeshMaterial_AlphaTest` is `MeshMaterialAlphaTest`; `AssetTypeName` is `kAssetTypeName`.

### Removed
- Reading version 3 mesh files (before 0.17.0). The editor builds them again.
- The loader for `Camera3DComponent`, the 3D camera of scenes saved before
  0.17.0. Open and save such a scene with 0.17 first.
- The former names from before 0.16.0 (bare class names, and deki-gpio's
  `DekiEsp32::ESP32PinSetup`). A scene that old is upgraded with 0.17 first.

## 0.17.0

### Added
- A model's texture takes Max Size from its `.data` file (per target, like an
  image's): the stored powers of two are capped by it, and by 256 when it has
  none. `CompileObjToMesh` takes `maxTextureSize`. The per-target entry's
  object form (`{"format", "maxSize"}`) reads, as does the older string.
- **Every format.** Meshes draw into RGB565, RGB888, ARGB8888 and RGB565A8
  framebuffers, and textures can be RGB565, RGB565A8, RGB888, RGBA8888 or
  alpha-only. The RGB565-into-RGB565 path is the loop it always was. A texel
  with alpha below half is a hole when the material has alpha test, which the
  importer turns on for a texture with transparent pixels.
- Imported models store their texture in the format their `.data` settings
  and the target ask for (Automatic follows the target's screen), and export
  per target. Mesh files are version 4 (a format byte per texture); version 3
  files still load as RGB565.
- `Deki3DTests`: every texture format into every framebuffer format, cutout,
  and the mesh file. The two standalone programs moved to `tests/standalone/`.

### Changed
- `minEngine` 0.17.0. Reflection ABI 20: the package must be rebuilt.
- **The scene camera holds the lens.** `Camera3DComponent` is now
  `Mesh3DSettings` (tiles, retro switches, light, threads); its field of view
  and clip planes moved to `DekiRendering::CameraComponent`, which draws meshes
  when its projection is Perspective. The editor moves an old scene's values
  onto the camera when it loads it.
- The field of view is vertical on every screen, like the orthographic
  camera's height, so meshes and sprites line up on every screen.

### Fixed
- A firmware build drew no 3D: the mesh render pass registers from a static
  object in a file nothing referenced, which a static link drops. The
  package's system init now keeps it (seen under QEMU with its new display:
  the test scene's model now draws, matching the editor).
- A mesh too big for a board's memory rebooted it: the loader read the file,
  and the asset kept its vertices, indices and texture, in std::vectors on the
  internal heap, which abort when full. They are Deki::Buffers now, the big
  ones External (PSRAM where the board has it): a mesh that does not fit logs
  "no room for ..." and does not load, and with PSRAM it loads there
  (verified under QEMU: 201 KB external, 1 KB internal).
- A firmware build has a Mesh loader. It was registered only by a static
  object in a file nothing referenced, which a static link drops;
  `Deki3D_InitSystem` (the package's system init) registers it now.
- Removed an `if` with an empty body from the rasteriser, which ESP-IDF 6's
  GCC 15 build rejects. It did nothing either way.

## 0.16.0

### Changed
- **Moved into the `Deki3D` namespace.** Every component was declared at global
  scope, which made its identity a bare class name — the name a scene file
  stores and the name the registry keys on — so two packages defining one name
  collided there with nothing to tell them apart. Each component carries
  `DEKI_FORMER_NAME` with the name it was saved under before, so existing
  scenes load unchanged and are written back qualified on the next save.
  Code naming these types needs the namespace: `using namespace Deki3D;` or a
  qualified name.
- Enum properties are stored by name rather than by number, so appending to an
  enum or reordering one no longer changes what a saved scene means. Files
  written before this still read.
- `minEngine` 0.16.0. Reflection ABI 17: the package must be rebuilt.

## 0.15.0

### Changed
- No changes of its own. Released alongside engine 0.15.0 so `minEngine`
  tracks the engine version.
