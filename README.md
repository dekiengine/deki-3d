# deki-3d

> **Experimental.** Published so it can be tried and talked about, not because
> it is settled. The component properties, the compiled mesh format and the
> render pass hooks may all change without a migration path, and it has been
> run on the desktop simulator and compiled for an ESP32-S3 but not yet run on
> real hardware. Pin a version if you depend on it.

Software-rasterised 3D for the Deki engine.

Geometry is drawn by a render pass, not by a replacement renderer, so a 3D
scene and a 2D interface share one framebuffer and one sort order. The
rasteriser bins triangles into tiles and keeps a depth buffer for one tile at
a time: 2 KB at a 32-pixel tile, rather than the 150 KB a full-screen depth
buffer would cost at 320x240.

Per-vertex work is float, per-pixel work is fixed point. On a part with no
hardware FPU that means emulation is only paid at vertex rate.

## Components

- `MeshComponent` draws an imported mesh, or a built-in primitive while no
  mesh asset is assigned. The object's transform places it.
- The scene camera (`DekiRendering::CameraComponent`) draws meshes when its
  Projection is Perspective; its field of view and clip planes live there.
  With an orthographic camera, meshes are not drawn.
- `Mesh3DSettings` sets how the pass draws: tile size, the retro switches, the
  light and the fill threads. Put one anywhere in the scene; without one the
  pass uses its declared defaults. It was `Camera3DComponent` before 0.18, and
  the editor moves an old one's field of view onto the camera when it loads the
  scene.

## Models

Drop a `.obj` into the project. The editor compiles it to a flat binary in the
asset cache and a `MeshComponent` can then point at it. The `mtllib` it names
is read too, and the first `map_Kd` becomes the mesh's texture, stored inside
the compiled mesh as RGB565 at a power-of-two size. Faces may be polygons,
indices may be negative, `usemtl` starts a submesh, and normals are generated
when the file has none.

Two limits worth knowing: 65535 vertices per mesh, because the index buffer is
16-bit, and one texture per mesh, so a model with several materials draws them
all with the first.

## Seeing it in the editor

The editor's viewport has its own camera with no 3D settings, so the pass
borrows the lens of the scene's perspective camera. The preview looks straight
down -Z, pulled back so the plane at z = 0 covers what the viewport shows in
2D. An object at z = 0 is the same size as 2D content of the same size, and
zooming scales both. Anything further back is smaller.

## Threads

`Mesh3DSettings.fillThreads` splits the fill across cores on a device or
simulator build. Tiles are independent, so the output is identical whatever the
count; only the speed changes. The editor always previews with one thread,
because it loads packages as unloadable DLLs and joining a thread while one
unloads deadlocks the Windows loader.

## The retro switches

`perspectiveCorrect` off gives you PlayStation-era texture warping,
`vertexSnap` gives you its vertex jitter. Both are there for the look: correct
texturing divides once per span rather than per pixel, so affine only buys
about a tenth.

## Requires

`deki-rendering`, and a project at the 3D transform width (this package's
`transform_3d` tag arranges that).

## Dependencies

| Dependency | Type |
|---|---|
| `deki-rendering` | Deki package |

## Namespace

Types live in `Deki3D`. Scene files store the qualified name, and so does code:

```cpp
using namespace Deki3D;
obj->AddComponent<SomeComponent>();
```

Scenes saved before 0.16.0 used bare names and still load; saving writes the current one.

