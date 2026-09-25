# Changelog

Notable changes to `deki-3d`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### Added
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
- **The scene camera holds the lens.** `Camera3DComponent` is now
  `Mesh3DSettings` (tiles, retro switches, light, threads); its field of view
  and clip planes moved to `DekiRendering::CameraComponent`, which draws meshes
  when its projection is Perspective. The editor moves an old scene's values
  onto the camera when it loads it.
- The field of view applies to the design area's shape and follows the
  project's Screen Fit, so meshes and sprites line up on every screen.

### Fixed
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
