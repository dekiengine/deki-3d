/**
 * @file MeshRoundTrip.cpp
 * @brief .obj text in, compiled blob out, MeshAsset back in, rasterised.
 *
 * Checks the importer and the loader against each other without an editor, an
 * asset cache or a project, which is what makes a malformed-input case cheap
 * to write. The malformed cases matter more than the happy one: a mesh blob
 * is parsed on a device with no memory protection worth the name.
 *
 * Build (MSYS2 mingw64), from tests/:
 *   g++ -std=c++17 -O2 -DDEKI_EDITOR -I<engine>/include -I.. \
 *       MeshRoundTrip.cpp ../editor/ObjImporter.cpp ../MeshAsset.cpp ../Raster3D.cpp -o meshtest
 */

#include "../MeshAsset.h"
#include "../Math3D.h"
#include "../Raster3D.h"
#include "../editor/ObjImporter.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Deki3D;

namespace
{
int g_Failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_Failures;
}

// A unit cube: eight corners, six quad faces, normals and texture
// coordinates left out so the importer has to generate the normals.
const char* kCubeObj = R"(# a cube
v -1 -1  1
v  1 -1  1
v  1  1  1
v -1  1  1
v -1 -1 -1
v  1 -1 -1
v  1  1 -1
v -1  1 -1
usemtl front
f 1 2 3 4
usemtl back
f 6 5 8 7
f 2 6 7 3
f 5 1 4 8
f 4 3 7 8
f 5 6 2 1
)";
}  // namespace

int main()
{
    std::printf("Obj import\n");

    std::vector<uint8_t> blob;
    std::string error;
    Check(CompileObjToMesh(kCubeObj, blob, error), "a cube compiles");
    if (!error.empty())
        std::printf("        error: %s\n", error.c_str());

    MeshAsset mesh;
    Check(mesh.LoadFromMemory(blob.data(), blob.size()), "the blob loads back");
    Check(mesh.Valid(), "the mesh is usable");
    Check(mesh.VertexCount() == 8, "eight corners, shared between faces");
    Check(mesh.TriangleCount() == 12, "six quads fan into twelve triangles");

    const Mesh3D& view = mesh.View();
    Check(view.submeshCount == 2, "two usemtl groups make two submeshes");
    Check(view.layout.HasNormals(), "normals were generated");
    Check(!view.layout.HasUVs(), "no texture coordinates in, none out");
    Check(view.boundsMin.x == -1.0f && view.boundsMax.z == 1.0f, "bounds cover the cube");

    // Generated normals should point outward: the corner at (1,1,1) averages
    // three outward faces, so every component is positive.
    {
        const auto* v = static_cast<const float*>(view.VertexAt(2));
        const float* n = v + 3;
        Check(n[0] > 0.0f && n[1] > 0.0f && n[2] > 0.0f, "generated normals point outward");
    }

    std::printf("\nMalformed input is refused, not trusted\n");
    {
        std::vector<uint8_t> junk = blob;
        junk.resize(blob.size() / 2);
        MeshAsset m;
        Check(!m.LoadFromMemory(junk.data(), junk.size()), "a truncated blob is refused");
    }
    {
        std::vector<uint8_t> bad = blob;
        bad[0] = 'X';
        MeshAsset m;
        Check(!m.LoadFromMemory(bad.data(), bad.size()), "a bad magic is refused");
    }
    {
        std::vector<uint8_t> bad = blob;
        MeshFileHeader h{};
        std::memcpy(&h, bad.data(), sizeof(h));
        h.version = kMeshFileVersion + 1;
        std::memcpy(bad.data(), &h, sizeof(h));
        MeshAsset m;
        Check(!m.LoadFromMemory(bad.data(), bad.size()), "a future version is refused");
    }
    {
        // Point the first index past the end of the vertex buffer. Left
        // unchecked this is an out-of-bounds read inside the span loop.
        std::vector<uint8_t> bad = blob;
        MeshFileHeader h{};
        std::memcpy(&h, bad.data(), sizeof(h));
        const size_t indexOffset =
            sizeof(MeshFileHeader) + static_cast<size_t>(h.vertexCount) * h.vertexStride;
        const uint16_t rogue = 9999;
        std::memcpy(bad.data() + indexOffset, &rogue, sizeof(rogue));
        MeshAsset m;
        Check(!m.LoadFromMemory(bad.data(), bad.size()), "an index past the vertices is refused");
    }
    {
        std::string err;
        std::vector<uint8_t> out;
        Check(!CompileObjToMesh("v 0 0 0\nv 1 0 0\n", out, err), "a file with no faces is refused");
        Check(!CompileObjToMesh("v 0 0 0\nf 1 2 3\n", out, err), "an out-of-range face is refused");
    }

    std::printf("\nIt rasterises\n");
    {
        const int W = 96, H = 96;
        std::vector<uint16_t> fb(static_cast<size_t>(W) * H, 0);
        Material3D mat;
        mat.shading = ShadingModel::VertexLit;

        RasterConfig cfg;
        cfg.tileSize = 32;
        Raster3D r;
        const Deki::Mat4 vp = Mul(Perspective(1.0f, 1.0f, 0.1f, 100.0f),
                                  LookAt(Deki::Vector3(0, 0, 5), Deki::Vector3(0, 0, 0),
                                         Deki::Vector3(0, 1, 0)));
        r.BeginFrame(reinterpret_cast<uint8_t*>(fb.data()), W, H, Deki::ColorFormat::RGB565, cfg);
        r.DrawMesh(view, &mat, 1, Mul(vp, Compose(0, 0, 0, 0.4f, 0.7f, 0, 1, 1, 1)),
                   Deki::Mat4::Identity());
        r.EndFrame();

        Check(r.Stats().trianglesIn == 12, "all twelve triangles reached the rasteriser");
        Check(r.Stats().pixelsWritten > 500, "an imported mesh fills pixels");
    }

    std::printf("\n%d check(s) failed\n", g_Failures);
    return g_Failures ? 1 : 0;
}
