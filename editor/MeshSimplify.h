#pragma once

#ifdef DEKI_EDITOR

#include <cstdint>
#include <vector>

namespace Deki3D
{

/// One triangle of a mesh being reduced: three vertex indices and the
/// submesh (material range) it belongs to.
struct SimplifyTriangle
{
    uint32_t v[3];
    uint32_t submesh;
};

/// Reduces `triangles` toward `targetTriangles` by collapsing edges, the one
/// that moves the surface least first (each vertex's quadric error, as
/// Garland and Heckbert describe). A collapse moves one vertex onto a
/// neighbour, so every kept vertex keeps its own position, normal and
/// texture coordinates.
///
/// A vertex on an open edge, on a seam (where the texture coordinates or
/// normals change, so the mesh has two vertices at one place) or between two
/// materials never moves: moving one side of a seam would open a crack. So a
/// mesh made of many separate pieces may stop above the target; the result
/// says how many triangles are left. No collapse flips a triangle over.
///
/// `positions` is x, y, z per vertex. Triangles that are left keep their
/// order within each submesh. Vertices no triangle uses any more are left in
/// place: the caller drops them.
void SimplifyMesh(const std::vector<float>& positions, std::vector<SimplifyTriangle>& triangles,
                  uint32_t targetTriangles);

}  // namespace Deki3D

#endif  // DEKI_EDITOR
