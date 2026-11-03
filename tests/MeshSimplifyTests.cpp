// Tests the triangle reduction a target's Detail asks for: it gets near the
// count, keeps the shape, flips nothing, and never moves a seam, an open edge
// or a material border. And the model settings that ask for it.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "../MeshAsset.h"
#include "../editor/MeshSimplify.h"
#include "../editor/ModelSettings.h"
#include "../editor/ObjImporter.h"
#include <deki-editor/TextureImporter.h>

using namespace Deki3D;

namespace
{

struct Mesh
{
    std::vector<float> positions;
    std::vector<SimplifyTriangle> triangles;

    uint32_t Add(float x, float y, float z)
    {
        positions.insert(positions.end(), { x, y, z });
        return static_cast<uint32_t>(positions.size() / 3 - 1);
    }
};

// A side x side grid of vertices in the z = 0 plane, two triangles a cell,
// the left half in submesh 0 and the right in submesh 1.
Mesh Grid(int side)
{
    Mesh m;
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            m.Add(static_cast<float>(x), static_cast<float>(y), 0.0f);
        }
    }
    for (int y = 0; y + 1 < side; ++y)
    {
        for (int x = 0; x + 1 < side; ++x)
        {
            const uint32_t a = y * side + x, b = a + 1, c = a + side + 1, d = a + side;
            const uint32_t sub = x < (side - 1) / 2 ? 0 : 1;
            m.triangles.push_back({ { a, b, c }, sub });
            m.triangles.push_back({ { a, c, d }, sub });
        }
    }
    return m;
}

// A sphere of rings x segments, with a seam: the last column of vertices sits
// on the first, as a texture's wrap would leave it.
Mesh Sphere(int rings, int segments)
{
    Mesh m;
    for (int r = 0; r <= rings; ++r)
    {
        const float phi = 3.14159265f * r / rings;
        for (int s = 0; s <= segments; ++s)
        {
            const float theta = 2.0f * 3.14159265f * s / segments;
            m.Add(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
        }
    }
    const int row = segments + 1;
    for (int r = 0; r < rings; ++r)
    {
        for (int s = 0; s < segments; ++s)
        {
            const uint32_t a = r * row + s, b = a + 1, c = a + row + 1, d = a + row;
            if (r > 0)
            {
                m.triangles.push_back({ { a, c, b }, 0 });
            }
            if (r + 1 < rings)
            {
                m.triangles.push_back({ { a, d, c }, 0 });
            }
        }
    }
    return m;
}

// Each triangle's normal, unnormalised.
void Normal(const Mesh& m, const SimplifyTriangle& t, double n[3])
{
    const float* a = &m.positions[t.v[0] * 3];
    const float* b = &m.positions[t.v[1] * 3];
    const float* c = &m.positions[t.v[2] * 3];
    const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const double v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
}

// The vertices on edges one triangle uses.
std::set<uint32_t> OpenEdgeVertices(const std::vector<SimplifyTriangle>& triangles)
{
    std::map<std::pair<uint32_t, uint32_t>, int> uses;
    for (const SimplifyTriangle& t : triangles)
    {
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t a = t.v[k], b = t.v[(k + 1) % 3];
            ++uses[{ std::min(a, b), std::max(a, b) }];
        }
    }
    std::set<uint32_t> out;
    for (const auto& [edge, n] : uses)
    {
        if (n == 1)
        {
            out.insert(edge.first);
            out.insert(edge.second);
        }
    }
    return out;
}

}  // namespace

TEST(MeshSimplify, AFlatGridComesDownAndStaysFlatAndFacingUp)
{
    Mesh m = Grid(40);
    const size_t before = m.triangles.size();
    const std::set<uint32_t> edgeBefore = OpenEdgeVertices(m.triangles);
    SimplifyMesh(m.positions, m.triangles, static_cast<uint32_t>(before / 10));

    EXPECT_LT(m.triangles.size(), before / 3) << "well down from " << before;
    for (const SimplifyTriangle& t : m.triangles)
    {
        double n[3];
        Normal(m, t, n);
        EXPECT_GT(n[2], 0.0) << "a triangle turned over";
    }
    // The outline and the border between the two materials are where they
    // were: every vertex on them is still used.
    std::set<uint32_t> used;
    for (const SimplifyTriangle& t : m.triangles)
    {
        used.insert(t.v, t.v + 3);
    }
    for (uint32_t v : edgeBefore)
    {
        EXPECT_TRUE(used.count(v)) << "open-edge vertex " << v << " was moved";
    }
    for (const SimplifyTriangle& t : m.triangles)
    {
        for (uint32_t v : t.v)
        {
            const float x = m.positions[v * 3];
            EXPECT_TRUE(t.submesh == 0 ? x <= 19.0f : x >= 19.0f) << "a triangle crossed the material border";
        }
    }
}

TEST(MeshSimplify, ASphereKeepsItsShapeAndSeam)
{
    Mesh m = Sphere(24, 32);
    const size_t before = m.triangles.size();
    const std::set<uint32_t> seamBefore = OpenEdgeVertices(m.triangles);
    // Whether a triangle faces the way its corner points, out from the centre.
    auto outward = [&](const SimplifyTriangle& t)
    {
        double n[3];
        Normal(m, t, n);
        const float* a = &m.positions[t.v[0] * 3];
        return n[0] * a[0] + n[1] * a[1] + n[2] * a[2] > 0.0;
    };
    const bool facesOut = outward(m.triangles[m.triangles.size() / 2]);
    for (const SimplifyTriangle& t : m.triangles)
    {
        ASSERT_EQ(outward(t), facesOut) << "the test sphere is wound one way";
    }
    SimplifyMesh(m.positions, m.triangles, static_cast<uint32_t>(before / 4));

    EXPECT_LT(m.triangles.size(), before / 2);
    for (const SimplifyTriangle& t : m.triangles)
    {
        EXPECT_EQ(outward(t), facesOut) << "a triangle turned over";
    }
    // The seam's open edges are still exactly the seam's.
    EXPECT_EQ(OpenEdgeVertices(m.triangles), seamBefore) << "the seam moved, which would open a crack";
}

TEST(MeshSimplify, NothingToDoLeavesTheMeshAlone)
{
    Mesh m = Grid(5);
    const std::vector<SimplifyTriangle> before = m.triangles;
    SimplifyMesh(m.positions, m.triangles, static_cast<uint32_t>(before.size()));
    ASSERT_EQ(m.triangles.size(), before.size());
}

TEST(MeshSimplify, TheImporterKeepsTheDetailAsked)
{
    std::ostringstream obj;
    const int side = 60;
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            // A gentle bump, so collapses cost something and go in order.
            const float z = std::sin(x * 0.1f) * std::cos(y * 0.1f);
            obj << "v " << x << " " << y << " " << z << "\n";
        }
    }
    for (int y = 0; y + 1 < side; ++y)
    {
        for (int x = 0; x + 1 < side; ++x)
        {
            const int a = y * side + x + 1;
            obj << "f " << a << " " << a + 1 << " " << a + side + 1 << " " << a + side << "\n";
        }
    }
    MeshCompileReport report;
    MeshCompileOptions options;
    options.detail = 0.25f;
    options.report = &report;
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(obj.str(), "", {}, blob, error, options)) << error;
    EXPECT_EQ(report.trianglesIn, static_cast<uint32_t>((side - 1) * (side - 1) * 2));
    EXPECT_LE(report.trianglesOut, report.trianglesIn * 3 / 10) << "near a quarter";

    MeshAsset mesh;
    ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
    EXPECT_EQ(mesh.TriangleCount(), report.trianglesOut);
    EXPECT_EQ(mesh.VertexCount(), report.verticesOut) << "vertices no triangle uses are dropped";
    EXPECT_LT(mesh.VertexCount(), static_cast<uint32_t>(side * side));
}

// A real model, named by DEKI_TEST_OBJ (Crytek Sponza, say), compiled as a GPU
// target and as a board stores it: the time each takes, and what comes out.
// Run by hand with --gtest_also_run_disabled_tests.
TEST(MeshSimplify, DISABLED_CompileAModel)
{
    const char* path = std::getenv("DEKI_TEST_OBJ");
    if (!path)
    {
        GTEST_SKIP() << "set DEKI_TEST_OBJ to an .obj";
    }
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in) << path;
    std::ostringstream text;
    text << in.rdbuf();
    const std::string dir = std::filesystem::path(path).parent_path().string();
    auto decode = [](const std::string& image, int& w, int& h, std::vector<uint8_t>& rgba)
    {
        DekiEditor::DecodedImage decoded;
        if (!DekiEditor::DecodeImageFile(image, decoded))
        {
            return false;
        }
        w = decoded.width;
        h = decoded.height;
        rgba = std::move(decoded.rgba);
        return true;
    };
    struct Case
    {
        const char* name;
        int maxTextureSize;
        float detail;
        TexelFormat format;
    };
    for (const Case& c : { Case{ "GPU, all of it", 0, 1.0f, TexelFormat::RGBA8888 },
                           Case{ "board, 256 and a tenth", 256, 0.1f, TexelFormat::RGB565 } })
    {
        MeshCompileReport report;
        MeshCompileOptions options;
        options.maxTextureSize = c.maxTextureSize;
        options.detail = c.detail;
        options.report = &report;
        options.chooseFormat = [&](bool hasAlpha)
        { return hasAlpha && c.format == TexelFormat::RGB565 ? TexelFormat::RGB565A8 : c.format; };
        std::vector<uint8_t> blob;
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        ASSERT_TRUE(CompileObjToMesh(text.str(), dir, decode, blob, error, options)) << error;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        MeshAsset mesh;
        ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
        std::printf("%s: %.1f s, %u -> %u triangles, %u vertices, %u textures, %.1f MB, %s indices\n", c.name, seconds,
                    report.trianglesIn, report.trianglesOut, report.verticesOut, mesh.TextureCount(),
                    blob.size() / (1024.0 * 1024.0), mesh.View().indexBytes == 4 ? "32-bit" : "16-bit");
    }
}

// A model's settings go into its .data and come back, per target, and leave
// the rest of the file as it was.
TEST(ModelSettings, WriteAndReadBack)
{
    const std::string model = testing::TempDir() + "/settings_model.obj";
    {
        std::ofstream data(model + ".data", std::ios::trunc);
        data << R"({"guid": "abc", "settings": {"texture": {"chroma_key": {"enabled": true}}}})";
    }
    ModelSettings s;
    s.texture.maxSize = 512;
    s.texture.targets["lilygo_t_deck"].maxSize = 128;
    s.filter = "Smooth";
    s.detail = 0.5f;
    s.targets["lilygo_t_deck"].filter = "Point";
    s.targets["lilygo_t_deck"].detail = 0.1f;
    ASSERT_TRUE(WriteModelSettings(model, s));

    const ModelSettings back = ReadModelSettings(model);
    EXPECT_EQ(back.texture.MaxSizeFor("native_simulator"), 512);
    EXPECT_EQ(back.texture.MaxSizeFor("lilygo_t_deck"), 128);
    EXPECT_TRUE(back.SmoothFor("native_simulator"));
    EXPECT_FALSE(back.SmoothFor("lilygo_t_deck"));
    EXPECT_FLOAT_EQ(back.DetailFor("native_simulator"), 0.5f);
    EXPECT_FLOAT_EQ(back.DetailFor("lilygo_t_deck"), 0.1f);

    std::ifstream in(model + ".data");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("\"guid\": \"abc\""), std::string::npos) << "the asset's own keys stay";
    EXPECT_NE(text.find("chroma_key"), std::string::npos) << "texture settings it does not know stay";

    // Back to the defaults: the model's part of the file goes.
    ASSERT_TRUE(WriteModelSettings(model, ModelSettings{}));
    std::ifstream again(model + ".data");
    const std::string cleared((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    EXPECT_EQ(cleared.find("\"model\""), std::string::npos);
}
