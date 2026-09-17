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

Float does the per-vertex work and fixed point does the per-pixel work, which
is what a span loop wants on any target and means parts without a hardware
FPU only pay emulation at vertex rate.

## Components

- `MeshComponent` draws an imported mesh, or a built-in primitive while no
  mesh asset is assigned. The object's transform places it.
- `Camera3DComponent` gives the scene camera a perspective projection. Without
  one, meshes are not drawn.

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

The editor's viewport draws through its own camera, which carries no 3D
settings, so the pass borrows them from the scene's `Camera3DComponent`
wherever it is. The preview looks straight down -Z from wherever you have
panned, pulled back so that the plane through z = 0 covers exactly what the
viewport is showing in 2D. An object of a given world size at z = 0 therefore
appears the same size as 2D content of that size, and zooming scales both
together. Geometry further back is smaller, as perspective requires.

## Threads

`Camera3DComponent.fillThreads` splits the fill across cores on a device or
simulator build. Tiles are independent, so the output is identical whatever the
count; only the speed changes. The editor always previews with one thread,
because it loads packages as unloadable DLLs and joining a thread while one
unloads deadlocks the Windows loader.

## The retro switches

`perspectiveCorrect` off gives the texture warping of PlayStation-era
hardware, and `vertexSnap` gives its vertex jitter. Neither is a performance
tier: correct texturing divides once per span rather than once per pixel, so
affine measures about a tenth cheaper. They are there because the look is
worth having.

## Requires

`deki-rendering`, and a project at the 3D transform width (this package's
`transform_3d` tag arranges that).

## Dependencies

| Dependency | Type |
|---|---|
| `deki-rendering` | Deki package |

## Namespace

This package's types live in `Deki3D`. Scene files store the qualified
name, so a component is `Deki3D::SomeComponent` there, and code naming one
needs the namespace:

```cpp
using namespace Deki3D;
obj->AddComponent<SomeComponent>();
```

Scenes saved before 0.16.0 used bare names and still load: every component
records what it used to be called, and a save writes the current name.

