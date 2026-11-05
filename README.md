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
- `Mesh3DSettings` sets how the pass draws: the renderer, tile size, the retro
  switches and the fill threads. Put one anywhere in the scene; without one the
  pass uses its declared defaults.
- The scene's `DekiRendering::Light` components light the meshes (below,
  "Lights"). A scene with none draws its lit meshes unlit, and says so.

## Lights

Lit meshes (VertexLit, Flat) are lit by the scene's Lights: the ambient, and
up to 8 directional, point and spot lights, the strongest if there are more
(the ones left out are named once). A surface takes each light by how near
it is and how much it faces it, in colour, up to twice its own. A spot faces
along its object's X axis, as in 2D.

- The software rasteriser lights each vertex (a flat face at its middle) by
  the 4 lights that reach the mesh most, and a triangle by its first vertex.
- The GPU lights each vertex the same way and blends across the triangle.
- Shadows, GPU only: a directional or spot light with Cast Shadows, at most
  two in a frame, casts the shadows of every mesh with `castShadows` on (the
  default). Each light's shadow map is drawn before the frame's meshes, with
  each mesh's own shader, so a `Displace()` or a texture's holes shape its
  shadow too; a lookup compares depth over the four nearest texels, for a
  softened edge. The software rasteriser, and so a board, draws the light
  without shadows, and says so; point lights cast none yet.

A scene saved with Mesh3DSettings' old light (`lightYaw`, `lightPitch`,
`ambient`) opens with two Lights in its place on the same object, an Ambient
one and a Directional one, that light it exactly as before.

## Models

Drop a `.obj` into the project. The editor compiles it to a flat binary in the
asset cache and a `MeshComponent` can then point at it. The `mtllib` it names
is read too: each material's `map_Kd` becomes its texture, and a `map_d`
beside it the texture's cutout. Faces may be polygons, indices may be
negative, `usemtl` starts a submesh, paths may use either slash, and normals
are generated when the file has none.

A mesh of any size compiles. Its indices are 16-bit while it has at most
65535 vertices and 32-bit past that, so only a big mesh pays for the larger;
both renderers draw either.

### Each platform gets its own build

The model keeps everything; each platform's build of it keeps what that
platform can use. The Model inspector shows what the editor's build holds,
then the Targets cards, Default for every platform and one per platform the
project can build for, which may differ from it:

- **Detail**: the share of the triangles kept (All, 75% down to 1%). The
  mesh is reduced by collapsing the edges that change its shape least; its
  seams, open edges and material borders stay where they are, so a model of
  many pieces may keep more than asked, and the build says how many.

It is kept in the model's `.data` (`settings.model`).

A model's textures are images of the project, and each is stored and drawn
as its image's own settings say, per platform, in the image's inspector:
**Format**, **Max Size** (the stored sides are the largest powers of two
within it) and **Filter** (Point, Bilinear or Trilinear; a platform without a
GPU draws Point). Select several images in the Asset Browser to set them
together. A change to an image, or to its settings, rebuilds the models that
use it.

The editor's own build follows the active platform, so the viewport shows
what that platform gets.

A board build checks that each model fits: a mesh needs its vertices,
indices and textures in the board's external memory, and one that needs more
than the board has stops the build, naming the model, what it needs and what
the board has. A board that declares no memory is said to be unchecked.

### Drawing in software

A board without OpenGL draws meshes with the software rasteriser. It collects
the frame's triangles, bins them into tiles (32x32 on a microcontroller,
below) and fills tile by tile; past
`RasterConfig::batchTriangles` it fills what it has and starts again against a
depth buffer for the whole screen, so a mesh of any size draws in fixed
memory, in PSRAM where the board has it. Parts of a mesh out of view are
skipped, triangles facing away are dropped before they are lit, and each
vertex is transformed once per part. Set the scene's 3D Settings
`fillThreads` to 2 on a dual-core board to draw on both cores.

Sponza at Detail 10% (82k triangles) draws at about 1 frame a second on a
LilyGO T-Deck; `RasterStats` says where the time goes.

## Seeing it in the editor

The editor's viewport has its own camera with no 3D settings, so the pass
borrows the lens of the scene's perspective camera. The preview looks straight
down -Z, pulled back so the plane at z = 0 covers what the viewport shows in
2D. An object at z = 0 is the same size as 2D content of the same size, and
zooming scales both. Anything further back is smaller.

## Threads

`Mesh3DSettings.fillThreads` splits the drawing across cores on a device or
simulator build. Tiles are independent, so the fill needs no coordination. A
part of a mesh with a few hundred triangles or more also has its geometry
(transform, lighting, clipping, projection) shared out in blocks, each thread
writing its triangles into its own memory, which the calling thread then bins
in their original order. The output is identical whatever the count; only the
speed changes. The editor always previews with one thread,
because it loads packages as unloadable DLLs and joining a thread while one
unloads deadlocks the Windows loader.

## Tiles

The software rasteriser's tile size is the platform's unless the scene fixes
one (`Mesh3DSettings.tiles`, Platform or Fixed). A platform sets its own with
a define in its platform file, `"defines": ["DEKI_3D_TILE_SIZE=64"]`, and one
that names none gets 32, whose depth buffer (2 KB a thread) a microcontroller
keeps in its internal RAM. The Miyoo Mini's file names 64, which beat 32 there
(a 6k-triangle scene from 52.8 to 56.6 frames a second in RGB565, 58.0 to
59.7 in ARGB8888), and the desktop simulator's 128; the editor previews at
128. The inspector shows what each of the project's targets uses. The picture
is the same at any size; only the speed changes.

A scene saved before had one size for every platform, saved as 32 unless
changed: with 32 it now takes the platform's, as a new scene does, and with
any other size it keeps it, Fixed.

## The retro switches

`perspectiveCorrect` off gives you PlayStation-era texture warping,
`vertexSnap` gives you its vertex jitter. Both are there for the look: correct
texturing divides once per span rather than per pixel, so affine only buys
about a tenth.

## On the GPU

Where the platform provides `opengl`, the same meshes draw on the GPU (OpenGL
ES 3.0, or desktop OpenGL 3.3 on Windows): the same picture, many times
faster, and nothing to install. The backend (`GLMesh3DBackend`) is compiled
only there (`DEKI_CAPABILITY_OPENGL`); a board without a GPU builds the
software rasteriser alone. The pass talks to either through `Mesh3DBackend`.

> **Experimental.** Run in the editor and a desktop simulator build on Windows
> (desktop OpenGL 3.3); its tests compare it with the software rasteriser
> picture for picture. Not yet run on a handheld.

### How it fits

The 2D content and the meshes share one sort order, as with the software
rasteriser, and the scene format is unchanged. How the meshes reach the
screen depends on the display:

- **Into the presented frame.** Where the display presents through
  deki-rendering's GL presenter (the desktop simulator does), the 2D drawn before a batch goes up as a layer at its first
  mesh, the meshes draw straight into the frame the presenter is building,
  and the CPU buffer starts over transparent for the 2D after them. Nothing
  is read back.
- **Read back.** Everywhere else (the editor, a display that shows a CPU
  buffer), each batch draws into an off-screen framebuffer cleared to
  transparent, and its end reads back the rectangle the meshes can have
  touched and writes the pixels they drew over the 2D below.

The log says which, once. OpenGL itself, and the context, come from
deki-rendering: loaded at run time, so a machine without a driver logs why and its
meshes draw in software.

### Choosing the renderer

`Mesh3DSettings.renderer` on the scene:

- **Auto**, the default: the GPU where the platform has OpenGL and a
  driver, the software rasteriser everywhere else.
- **Software**: always the rasteriser. In the editor, this is the exact
  picture a board without a GPU shows.
- **GPU**: the GPU. Where there is none it still draws, in software, and says
  so once.

The editor always has OpenGL, so on Auto it previews on the GPU even while
the active platform is a microcontroller. Pick Software to see that
board's picture.

### What it draws

What the software rasteriser draws: the three shading models (Unlit,
VertexLit, Flat) against the scene's lights, textures in every `Texture3D`
format with the same holes (palette index 0, RGB565 magenta, alpha below half)
when the material asks for the alpha test, and the retro switches
(`perspectiveCorrect` off for affine texturing, `vertexSnap`).

Where the GPU does better at no cost, it does: lighting and vertex colours are
interpolated across a triangle, where the rasteriser takes its first vertex's,
and light is not cut to 32 levels. On a cube, where each face has one normal,
the pictures are the same.

As in the rasteriser, a textured mesh draws its texels alone: the material
tint and vertex colours apply to untextured and coverage-only (ALPHA8)
meshes.

### Shaders

A `MeshComponent` can name a `.shader` (deki-rendering's `ShaderAsset`): HLSL
that defines either hook or both.

```hlsl
// A mesh that breathes and glows toward uColor.
float3 Displace(float3 position, float3 normal)
{
    return position + normal * sin(uTime * uParams.y) * uParams.x;
}

float4 Shade(Fragment f)
{
    float glow = 0.5 + 0.5 * sin(uTime * uParams.y);
    return float4(lerp(f.base.rgb, uColor.rgb * f.light, glow), 1.0);
}
```

- `Displace(position, normal)`, optional, returns the object-space position.
- `Shade(Fragment f)`, optional, returns the colour. `f.base` is what the
  shading model would draw; `f.albedo` (texel or vertex colour times tint,
  alpha the coverage), `f.uv`, `f.normal` and `f.position` (world space),
  `f.light` (per channel, 1 the colour as it is) and `f.pixel` are there to
  build on. `LightAt(position, normal)` is the scene's light at a world
  position, shadows and all, for a shader that lights per pixel.
- Uniforms: `uTime` (seconds), `uParams` and `uColor` (the component's
  `shaderParams` and `shaderColor`), `uAmbient`, the light arrays, `uTexture`.
  Matrices are row-major: `mul(M, v)`.

The editor compiles the file to GLSL for each device that draws it
(deki-rendering's README, "Shaders"); one that does not compile is an error
there, at the file's line. A build for a board without a GPU stops if a scene
it ships uses a shader. The built-in program, for meshes without a shader, is
the same HLSL with no hooks (`editor/MeshShaderUse.cpp`), compiled into
`GLMesh3DBuiltinShader.inl`; `GLBackend.TheBuiltInShaderIsWhatTheMeshUseCompilesTo`
fails when that is stale, and rewrites it with `DEKI_UPDATE_GENERATED=1`.

### Uploads

Meshes and textures are uploaded once per `revision` (`Mesh3D::revision`,
`Texture3D::revision`) and dropped after ten seconds unused. Mesh assets and
the built-in primitives set it; geometry built by hand with revision 0 is
uploaded every draw.

### Platforms

A platform gets the GPU backend when it provides `opengl`: its build then
defines `DEKI_CAPABILITY_OPENGL`. The desktop simulator platform does; a
handheld platform will once it exists. A board without it compiles none of
the GPU code.

### Speed

On a desktop (RTX 2080), 400 vertex-lit cubes at 640x480: software 10.5 ms a
frame, GPU 1.4 ms with the readback. The test
`GLBackend.DISABLED_BenchmarkAgainstSoftware` measures it on any machine:

```
Deki3DTests --gtest_also_run_disabled_tests --gtest_filter=*Benchmark*
```

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

## Community

Questions, ideas and what you made with Deki: [the Deki Discord](https://dekiengine.com/discord/).
