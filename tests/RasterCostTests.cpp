// What a frame of the software rasteriser costs, and that it is the same
// frame every time. Each case prints its geometry and fill time and a hash
// of its pixels, so a change to the rasteriser can be compared before and
// after:
//   Deki3DTests --gtest_filter=RasterCost.*

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../Math3D.h"
#include "../Raster3D.h"

using namespace Deki3D;

namespace
{

// A grid of small cubes, each turned its own way: many lit triangles, most
// of the frame left to the background.
struct Scene
{
    std::vector<VertexPNTC> vertices;
    std::vector<uint16_t> indices;
    Submesh3D sub;
    Mesh3D mesh;
};

Scene MakeScene()
{
    Scene s;
    const Deki::Vector3 normals[6] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
    };
    const Deki::Vector3 us[6] = { { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 1, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    const Deki::Vector3 vs[6] = { { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 } };
    const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    int cube = 0;
    for (int gy = 0; gy < 8; ++gy)
    {
        for (int gx = 0; gx < 12; ++gx, ++cube)
        {
            const Deki::Mat4 place =
                Mul(Translate(-4.4f + static_cast<float>(gx) * 0.8f, -2.8f + static_cast<float>(gy) * 0.8f,
                              -0.5f * static_cast<float>((gx + gy) % 4)),
                    Mul(RotateY(0.5f + static_cast<float>(cube) * 0.7f), RotateX(0.3f * static_cast<float>(cube))));
            for (int f = 0; f < 6; ++f)
            {
                const uint16_t base = static_cast<uint16_t>(s.vertices.size());
                for (int k = 0; k < 4; ++k)
                {
                    const Deki::Vector3 p = normals[f] + us[f] * corners[k][0] + vs[f] * corners[k][1];
                    float w = 1.0f;
                    VertexPNTC v;
                    v.position = TransformPoint(place, p * 0.22f, w);
                    v.normal = TransformDirection(place, normals[f]);
                    v.u = corners[k][0];
                    v.v = corners[k][1];
                    v.color = 0xFF000000u | ((40u + static_cast<uint32_t>(cube) * 17u) & 0xFF) << 16 |
                              ((90u + static_cast<uint32_t>(f) * 31u) & 0xFF) << 8 | 200u;
                    s.vertices.push_back(v);
                }
                for (uint16_t i : { 0, 1, 2, 0, 2, 3 })
                {
                    s.indices.push_back(static_cast<uint16_t>(base + i));
                }
            }
        }
    }
    s.sub.indexCount = static_cast<uint32_t>(s.indices.size());
    s.mesh.vertices = s.vertices.data();
    s.mesh.vertexCount = static_cast<uint32_t>(s.vertices.size());
    s.mesh.layout = LayoutPNTC();
    s.mesh.indices = s.indices.data();
    s.mesh.indexCount = static_cast<uint32_t>(s.indices.size());
    s.mesh.submeshes = &s.sub;
    s.mesh.submeshCount = 1;
    s.mesh.boundsMin = Deki::Vector3(-5, -4, -3);
    s.mesh.boundsMax = Deki::Vector3(5, 4, 2);
    return s;
}

struct Frame
{
    uint64_t hash = 0;
    RasterStats stats;
};

// One frame over a background, lit per vertex.
Frame Draw(const Scene& scene, Deki::ColorFormat format, int width, int height, std::vector<uint8_t>& pixels)
{
    pixels.assign(Deki::FrameBufferBytes(format, width, height), 0);
    Material3D material;
    material.shading = ShadingModel::VertexLit;
    RasterConfig config;
    const Deki::Mat4 projection =
        Perspective(1.0f, static_cast<float>(width) / static_cast<float>(height), 0.1f, 50.0f);
    const Deki::Mat4 view = LookAt(Deki::Vector3(0.4f, 0.8f, 7.0f), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 1, 0));
    const uint8_t background[4] = { 0x31, 0x52, 0x73, 0xFF };
    Raster3D raster;
    raster.BeginFrame(pixels.data(), width, height, format, config);
    raster.SetBackground(background);
    raster.DrawMesh(scene.mesh, &material, 1, Mul(projection, view), Deki::Mat4::Identity());
    raster.EndFrame();

    Frame frame;
    frame.stats = raster.Stats();
    frame.hash = 1469598103934665603ull;
    for (uint8_t byte : pixels)
    {
        frame.hash = (frame.hash ^ byte) * 1099511628211ull;
    }
    return frame;
}

void Check(const char* name, Deki::ColorFormat format)
{
    const Scene scene = MakeScene();
    std::vector<uint8_t> pixels;
    constexpr int kWidth = 320, kHeight = 240, kFrames = 200;
    const Frame first = Draw(scene, format, kWidth, kHeight, pixels);
    uint64_t geometry = 0, fill = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f)
    {
        const Frame again = Draw(scene, format, kWidth, kHeight, pixels);
        ASSERT_EQ(again.hash, first.hash) << name << ": the same frame drew other pixels";
        geometry += again.stats.geometryMicros;
        fill += again.stats.fillMicros;
    }
    const double total = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    std::printf("FRAMECOST %-9s geometry %7.1f us, fill %7.1f us, frame %7.1f us, pixels %016llx\n", name,
                static_cast<double>(geometry) / kFrames, static_cast<double>(fill) / kFrames, total / kFrames,
                static_cast<unsigned long long>(first.hash));
}

}  // namespace

TEST(RasterCost, LitCubesOverABackgroundInRgb565)
{
    Check("RGB565", Deki::ColorFormat::RGB565);
}

TEST(RasterCost, LitCubesOverABackgroundInArgb8888)
{
    Check("ARGB8888", Deki::ColorFormat::ARGB8888);
}
