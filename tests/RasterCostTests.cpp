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

// A 16 x 16 RGB565 texture of noise.
struct Texture
{
    std::vector<uint8_t> texels;
    Texture3D view;

    Texture()
    {
        texels.resize(16 * 16 * 2);
        for (size_t i = 0; i < texels.size(); ++i)
        {
            texels[i] = static_cast<uint8_t>(i * 37 + (i >> 3) * 11);
        }
        view.pixels = texels.data();
        view.format = TexelFormat::RGB565;
        view.width = view.height = 16;
        view.widthShift = view.heightShift = 4;
        view.hasAlpha = false;
    }
};

// One frame over a background, lit, with the texture when there is one.
Frame Draw(const Scene& scene, Deki::ColorFormat format, int width, int height, std::vector<uint8_t>& pixels,
           const Texture3D* texture = nullptr)
{
    pixels.assign(Deki::FrameBufferBytes(format, width, height), 0);
    Material3D material;
    material.shading = ShadingModel::VertexLit;
    material.texture = texture;
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

void Check(const char* name, Deki::ColorFormat format, const Texture3D* texture = nullptr)
{
    const Scene scene = MakeScene();
    std::vector<uint8_t> pixels;
    constexpr int kWidth = 320, kHeight = 240, kFrames = 200;
    const Frame first = Draw(scene, format, kWidth, kHeight, pixels, texture);
    uint64_t geometry = 0, fill = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f)
    {
        const Frame again = Draw(scene, format, kWidth, kHeight, pixels, texture);
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

TEST(RasterCost, TexturedLitCubesInRgb565)
{
    const Texture texture;
    Check("textured", Deki::ColorFormat::RGB565, &texture.view);
}

namespace
{
// A wall facing the eye, its texture going `turns` times round before it starts.
Scene MakeWall(float turns)
{
    Scene s;
    const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (const auto& corner : corners)
    {
        VertexPNTC v;
        v.position = Deki::Vector3(corner[0] * 2.0f, corner[1] * 1.5f, 0.0f);
        v.normal = Deki::Vector3(0, 0, 1);
        v.u = turns + (corner[0] + 1.0f);
        v.v = turns + (corner[1] + 1.0f);
        v.color = 0xFFFFFFFFu;
        s.vertices.push_back(v);
    }
    s.indices = { 0, 1, 2, 0, 2, 3 };
    s.sub.indexCount = 6;
    s.mesh.vertices = s.vertices.data();
    s.mesh.vertexCount = 4;
    s.mesh.layout = LayoutPNTC();
    s.mesh.indices = s.indices.data();
    s.mesh.indexCount = 6;
    s.mesh.submeshes = &s.sub;
    s.mesh.submeshCount = 1;
    s.mesh.boundsMin = Deki::Vector3(-2, -1.5f, -0.1f);
    s.mesh.boundsMax = Deki::Vector3(2, 1.5f, 0.1f);
    return s;
}
}  // namespace

// Texture coordinates far round a texture are past what a span's fixed point
// holds: they are brought back by whole turns, so the wall looks as it does
// near the texture's start. (They used to overflow, each processor its own way.)
TEST(RasterCost, ATextureFarRoundDrawsAsItDoesNear)
{
    const Texture texture;
    constexpr int kWidth = 320, kHeight = 240;
    std::vector<uint8_t> near, far;
    Draw(MakeWall(0.0f), Deki::ColorFormat::RGB565, kWidth, kHeight, near, &texture.view);
    // 4096 turns of a 16-texel texture: 65536 texels, twice what fits.
    Draw(MakeWall(4096.0f), Deki::ColorFormat::RGB565, kWidth, kHeight, far, &texture.view);
    ASSERT_EQ(near.size(), far.size());
    size_t same = 0;
    for (size_t i = 0; i < near.size(); i += 2)
    {
        same += near[i] == far[i] && near[i + 1] == far[i + 1] ? 1 : 0;
    }
    // All but the texels' edges, where so large a coordinate has lost its
    // last bits.
    EXPECT_GT(same * 100, near.size() / 2 * 90) << same << " of " << near.size() / 2 << " pixels the same";
}