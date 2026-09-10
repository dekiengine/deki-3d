# deki-3d

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

- `MeshComponent` draws a built-in primitive. The object's transform places it.
- `Camera3DComponent` gives the scene camera a perspective projection. Without
  one, meshes are not drawn.

## The retro switches

`perspectiveCorrect` off gives the texture warping of PlayStation-era
hardware, and `vertexSnap` gives its vertex jitter. Neither is a performance
tier: correct texturing divides once per span rather than once per pixel, so
affine measures about a tenth cheaper. They are there because the look is
worth having.

## Requires

`deki-rendering`, and a project at the 3D transform width (this package's
`transform_3d` tag arranges that).
