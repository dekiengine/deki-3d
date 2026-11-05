// Tests that the software rasteriser draws a frame of more triangles than one
// batch exactly as it draws it in one, depth included across batches, and
// that it skips a part of a mesh out of view without changing the picture.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../Math3D.h"
#include "../MeshAsset.h"
#include "../editor/ObjImporter.h"
#include "../Raster3D.h"
#include "deki-rendering/PixelFormat.h"

using namespace Deki3D;

namespace
{

constexpr int kSize = 64;
constexpr int kCells = 8;  // quads per layer side
constexpr Deki::ColorFormat kFormat = Deki::ColorFormat::RGB565;

// Layers of small quads at different depths, each layer its own colour, in
// an order that is neither front to back nor back to front, so a later batch
// holds both nearer and farther quads than an earlier one. `offsetX` moves
// them along x, so a part can be put outside the view.
struct Layers
{
    std::vector<VertexPNTC> vertices;
    std::vector<uint16_t> indices;

    void Add(float offsetX)
    {
        const float depths[4] = { 0.2f, -0.6f, 0.6f, -0.2f };
        const uint32_t colours[4] = { 0xFF2040E0u, 0xFF20E040u, 0xFFE04020u, 0xFFE0E020u };
        for (int layer = 0; layer < 4; ++layer)
        {
            // Each layer shifted by a quarter cell, so they overlap partly.
            const float shift = 0.06f * static_cast<float>(layer);
            for (int cy = 0; cy < kCells; ++cy)
            {
                for (int cx = 0; cx < kCells; ++cx)
                {
                    const float x0 = -1.0f + 2.0f * cx / kCells + shift + offsetX;
                    const float y0 = -1.0f + 2.0f * cy / kCells + shift;
                    const float size = 2.0f / kCells * 0.9f;
                    const uint16_t base = static_cast<uint16_t>(vertices.size());
                    const float z = depths[layer];
                    const Deki::Vector3 n(0, 0, 1);
                    vertices.push_back({ Deki::Vector3(x0, y0, z), n, 0.0f, 0.0f, colours[layer] });
                    vertices.push_back({ Deki::Vector3(x0 + size, y0, z), n, 0.0f, 0.0f, colours[layer] });
                    vertices.push_back({ Deki::Vector3(x0 + size, y0 + size, z), n, 0.0f, 0.0f, colours[layer] });
                    vertices.push_back({ Deki::Vector3(x0, y0 + size, z), n, 0.0f, 0.0f, colours[layer] });
                    for (uint16_t i : { 0, 1, 2, 0, 2, 3 })
                    {
                        indices.push_back(static_cast<uint16_t>(base + i));
                    }
                }
            }
        }
    }
};

struct Drawn
{
    std::vector<uint8_t> pixels;
    RasterStats stats;
};

// `before` is what the framebuffer holds first; `background`, when given,
// the pixel the rasteriser draws where it draws nothing.
Drawn Draw(const Layers& layers, const std::vector<Submesh3D>& submeshes, int batchTriangles, int threads = 1,
           uint16_t before = 0, const uint16_t* background = nullptr)
{
    Mesh3D mesh;
    mesh.vertices = layers.vertices.data();
    mesh.vertexCount = static_cast<uint32_t>(layers.vertices.size());
    mesh.layout = LayoutPNTC();
    mesh.indices = layers.indices.data();
    mesh.indexCount = static_cast<uint32_t>(layers.indices.size());
    mesh.submeshes = submeshes.data();
    mesh.submeshCount = static_cast<uint16_t>(submeshes.size());
    mesh.boundsMin = Deki::Vector3(-1, -1, -1);
    mesh.boundsMax = Deki::Vector3(6, 1.5f, 1);

    Material3D material;
    material.shading = ShadingModel::Unlit;
    material.doubleSided = true;

    Drawn drawn;
    drawn.pixels.resize(Deki::FrameBufferBytes(kFormat, kSize, kSize));
    for (size_t i = 0; i < drawn.pixels.size(); i += 2)
    {
        DekiPixel::Store565(&drawn.pixels[i], before);
    }
    RasterConfig config;
    config.tileSize = 16;
    config.batchTriangles = batchTriangles;
    config.threadCount = threads;
    Raster3D raster;
    raster.BeginFrame(drawn.pixels.data(), kSize, kSize, kFormat, config);
    if (background)
    {
        uint8_t pixel[2];
        DekiPixel::Store565(pixel, *background);
        raster.SetBackground(pixel);
    }
    raster.DrawMesh(mesh, &material, 1, Deki::Mat4::Ortho(-1, 1, -1, 1, -1, 1), Deki::Mat4::Identity());
    raster.EndFrame();
    drawn.stats = raster.Stats();
    return drawn;
}

Submesh3D Whole(const Layers& layers)
{
    Submesh3D sub;
    sub.indexCount = static_cast<uint32_t>(layers.indices.size());
    return sub;
}

}  // namespace

TEST(RasterBatches, ManyBatchesDrawWhatOneDoes)
{
    Layers layers;
    layers.Add(0.0f);
    const std::vector<Submesh3D> subs = { Whole(layers) };

    const Drawn one = Draw(layers, subs, 1 << 20);
    EXPECT_EQ(one.stats.batches, 1u);

    for (int threads : { 1, 2 })
    {
        SCOPED_TRACE(threads);
        const Drawn many = Draw(layers, subs, 7, threads);
        EXPECT_GT(many.stats.batches, 10u) << "drawn in batches of 7";
        EXPECT_EQ(many.stats.trianglesDropped, 0u);
        EXPECT_EQ(many.stats.trianglesClipped, one.stats.trianglesClipped);
        EXPECT_EQ(many.pixels, one.pixels) << "the same picture, depth tested across batches";
    }
}

TEST(RasterBatches, APartOutOfViewIsSkipped)
{
    Layers layers;
    layers.Add(0.0f);
    const uint32_t inView = static_cast<uint32_t>(layers.indices.size());
    layers.Add(4.0f);  // the second part, wholly right of the view
    const uint32_t all = static_cast<uint32_t>(layers.indices.size());

    Submesh3D first;
    first.indexCount = inView;
    Submesh3D second;
    second.firstIndex = inView;
    second.indexCount = all - inView;
    const std::vector<Submesh3D> unbounded = { first, second };

    std::vector<Submesh3D> bounded = unbounded;
    bounded[0].hasBounds = true;
    bounded[0].boundsMin = Deki::Vector3(-1, -1, -0.6f);
    bounded[0].boundsMax = Deki::Vector3(1.2f, 1.2f, 0.6f);
    bounded[1].hasBounds = true;
    bounded[1].boundsMin = Deki::Vector3(3, -1, -0.6f);
    bounded[1].boundsMax = Deki::Vector3(5.2f, 1.2f, 0.6f);

    const Drawn all_parts = Draw(layers, unbounded, 1 << 20);
    const Drawn culled = Draw(layers, bounded, 1 << 20);
    EXPECT_EQ(all_parts.stats.submeshesCulled, 0u) << "without bounds nothing is skipped";
    EXPECT_EQ(culled.stats.submeshesCulled, 1u);
    EXPECT_EQ(culled.stats.trianglesIn, all_parts.stats.trianglesIn) << "a skipped part still counts as handed in";
    EXPECT_EQ(culled.pixels, all_parts.pixels);
}

TEST(RasterBatches, WorkingMemoryCountsABatchAndTheFramesDepth)
{
    RasterConfig config;
    config.batchTriangles = 1000;
    const size_t small = Raster3D::WorkingBytes(320, 240, config);
    EXPECT_GT(small, static_cast<size_t>(320 * 240 * 2)) << "the whole-screen depth";
    config.batchTriangles = 2000;
    EXPECT_GT(Raster3D::WorkingBytes(320, 240, config), small + 1000 * 100) << "and each triangle of the batch";
}

// By hand: draws a compiled mesh file (DEKI_TEST_MESH, as a board build ships
// it) with the software rasteriser at 320x240 in RGB565, as a T-Deck does,
// from the camera DEKI_TEST_EYE gives ("x y z yaw pitch", degrees), once on
// one fill thread and once on two, into DEKI_TEST_OUT/one.ppm and two.ppm.
// Run with --gtest_also_run_disabled_tests.
TEST(RasterBatches, DISABLED_DrawACompiledMesh)
{
    // Or an .obj (DEKI_TEST_OBJ), compiled here at DEKI_TEST_DETAIL without
    // its textures, to see what Detail does to the shape.
    const char* path = std::getenv("DEKI_TEST_MESH");
    const char* obj = std::getenv("DEKI_TEST_OBJ");
    const char* out = std::getenv("DEKI_TEST_OUT");
    if ((!path && !obj) || !out)
    {
        GTEST_SKIP() << "set DEKI_TEST_MESH or DEKI_TEST_OBJ, and DEKI_TEST_OUT";
    }
    std::vector<uint8_t> blob;
    {
        FILE* f = std::fopen(path ? path : obj, "rb");
        ASSERT_NE(f, nullptr);
        uint8_t chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;)
        {
            blob.insert(blob.end(), chunk, chunk + n);
        }
        std::fclose(f);
    }
    if (!path)
    {
        MeshCompileOptions options;
        if (const char* d = std::getenv("DEKI_TEST_DETAIL"))
        {
            options.detail = static_cast<float>(std::atof(d));
        }
        const std::string text(blob.begin(), blob.end());
        std::string error;
        blob.clear();
        ASSERT_TRUE(CompileObjToMesh(text, "", {}, blob, error, options)) << error;
    }
    MeshAsset asset;
    ASSERT_TRUE(asset.LoadFromMemory(blob.data(), blob.size()));
    const Mesh3D& mesh = asset.View();
    std::printf("mesh: %u vertices, %u triangles, %u-byte indices, %u submeshes\n", mesh.vertexCount,
                mesh.indexCount / 3, mesh.indexBytes, mesh.submeshCount);

    float ex = 1150, ey = 180, ez = -40, eyaw = 90, epitch = 4.6f;
    if (const char* e = std::getenv("DEKI_TEST_EYE"))
    {
        std::sscanf(e, "%f %f %f %f %f", &ex, &ey, &ez, &eyaw, &epitch);
    }
    const float d2r = 3.14159265f / 180.0f;
    const int w = 320, h = 240;
    const Deki::Mat4 basis = Mul(RotateY(eyaw * d2r), RotateX(epitch * d2r));
    const Deki::Vector3 eye(ex, ey, ez);
    const Deki::Vector3 forward = TransformDirection(basis, Deki::Vector3(0, 0, -1));
    const Deki::Vector3 up = TransformDirection(basis, Deki::Vector3(0, 1, 0));
    const Deki::Mat4 viewProjection =
        Mul(Perspective(65.0f * d2r, static_cast<float>(w) / h, 5.0f, 6000.0f), LookAt(eye, eye + forward, up));

    std::vector<Material3D> materials(asset.Materials(), asset.Materials() + asset.MaterialCount());
    for (Material3D& m : materials)
    {
        m.shading = ShadingModel::VertexLit;
    }
    for (int threads : { 1, 2 })
    {
        RasterConfig config;
        config.threadCount = threads;
        std::vector<uint8_t> fb(Deki::FrameBufferBytes(kFormat, w, h), 0);
        Raster3D raster;
        raster.BeginFrame(fb.data(), w, h, kFormat, config);
        raster.DrawMesh(mesh, materials.data(), static_cast<uint16_t>(materials.size()), viewProjection,
                        Deki::Mat4::Identity(), Deki::Mat4::Identity());
        raster.EndFrame();
        const RasterStats& st = raster.Stats();
        std::printf("%d thread(s): %u in, %u drawn, %u parts skipped, %u batches, %u dropped\n", threads,
                    st.trianglesIn, st.trianglesClipped, st.submeshesCulled, st.batches, st.trianglesDropped);
        const std::string name = std::string(out) + (threads == 1 ? "/one.ppm" : "/two.ppm");
        FILE* ppm = std::fopen(name.c_str(), "wb");
        ASSERT_NE(ppm, nullptr);
        std::fprintf(ppm, "P6 %d %d 255\n", w, h);
        for (int i = 0; i < w * h; ++i)
        {
            uint8_t r, g, b, a;
            DekiPixel::ReadDstPixel<kFormat>(fb.data(), static_cast<size_t>(i), r, g, b, a);
            std::fputc(r, ppm);
            std::fputc(g, ppm);
            std::fputc(b, ppm);
        }
        std::fclose(ppm);
    }
}

// The geometry shared out between threads draws what one thread draws: the
// same triangles into the same tiles in the same order. A lit floor in
// perspective that runs from under the eye to the distance, so triangles are
// culled, near-clipped into two and lit, in batches that hold all of it, some
// of it in pieces the threads share, or too little for that.
TEST(RasterBatches, GeometryOnSeveralThreadsDrawsWhatOneDoes)
{
    constexpr int kGrid = 40;
    std::vector<VertexPNTC> vertices;
    std::vector<uint16_t> indices;
    for (int z = 0; z <= kGrid; ++z)
    {
        for (int x = 0; x <= kGrid; ++x)
        {
            const float fx = -10.0f + 20.0f * x / kGrid;
            const float fz = 2.0f - 40.0f * z / kGrid;  // from behind the eye to far ahead
            const uint32_t colour = 0xFF000000u | (((x * 37) & 0xFF) << 16) | (((z * 53) & 0xFF) << 8) | 0x80u;
            vertices.push_back({ Deki::Vector3(fx, 0.0f, fz), Deki::Vector3(0, 1, 0), 0.0f, 0.0f, colour });
        }
    }
    for (int z = 0; z < kGrid; ++z)
    {
        for (int x = 0; x < kGrid; ++x)
        {
            const uint16_t a = static_cast<uint16_t>(z * (kGrid + 1) + x);
            const uint16_t b = static_cast<uint16_t>(a + 1);
            const uint16_t c = static_cast<uint16_t>(a + kGrid + 2);
            const uint16_t d = static_cast<uint16_t>(a + kGrid + 1);
            for (uint16_t i : { a, b, c, a, c, d })
            {
                indices.push_back(i);
            }
        }
    }
    Submesh3D sub;
    sub.indexCount = static_cast<uint32_t>(indices.size());
    Mesh3D mesh;
    mesh.vertices = vertices.data();
    mesh.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.layout = LayoutPNTC();
    mesh.indices = indices.data();
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.submeshes = &sub;
    mesh.submeshCount = 1;
    mesh.boundsMin = Deki::Vector3(-10, 0, -38);
    mesh.boundsMax = Deki::Vector3(10, 0, 2);

    Material3D material;
    material.shading = ShadingModel::VertexLit;
    const Deki::Vector3 eye(0.3f, 1.0f, 0.0f);
    const Deki::Mat4 viewProjection =
        Mul(Perspective(1.2f, 1.0f, 0.1f, 100.0f), LookAt(eye, Deki::Vector3(0.0f, 0.2f, -5.0f), Deki::Vector3(0, 1, 0)));

    auto draw = [&](int batch, int threads)
    {
        Drawn drawn;
        drawn.pixels.assign(Deki::FrameBufferBytes(kFormat, kSize, kSize), 0);
        RasterConfig config;
        config.tileSize = 16;
        config.batchTriangles = batch;
        config.threadCount = threads;
        Raster3D raster;
        raster.BeginFrame(drawn.pixels.data(), kSize, kSize, kFormat, config);
        raster.DrawMesh(mesh, &material, 1, viewProjection, Deki::Mat4::Identity());
        raster.EndFrame();
        drawn.stats = raster.Stats();
        return drawn;
    };

    for (int batch : { 1 << 20, 1500, 700, 7 })
    {
        const Drawn one = draw(batch, 1);
        EXPECT_GT(one.stats.trianglesClipped, 1000u) << "most of the floor is in view";
        for (int threads : { 2, 3, 4 })
        {
            SCOPED_TRACE(::testing::Message() << batch << " triangles a batch, " << threads << " threads");
            const Drawn many = draw(batch, threads);
            EXPECT_EQ(many.pixels, one.pixels);
            EXPECT_EQ(many.stats.trianglesIn, one.stats.trianglesIn);
            EXPECT_EQ(many.stats.trianglesClipped, one.stats.trianglesClipped);
            EXPECT_EQ(many.stats.trianglesBinned, one.stats.trianglesBinned);
            EXPECT_EQ(many.stats.batches, one.stats.batches) << "the batches fill where they do on one thread";
            EXPECT_EQ(many.stats.trianglesDropped, 0u);
        }
    }
}

// Given the frame's clear (SetBackground), the rasteriser draws it wherever
// it draws nothing, tiles without a triangle included: the frame comes out as
// cleared first and drawn over, in one batch or many, on one thread or two.
TEST(RasterBatches, TheBackgroundIsWhatAClearWouldHaveLeft)
{
    Layers layers;
    layers.Add(0.7f);  // the left of the view holds nothing
    const std::vector<Submesh3D> subs = { Whole(layers) };
    const uint16_t clear = 0x39E7;
    for (int batch : { 100000, 37 })
    {
        for (int threads : { 1, 2 })
        {
            const Drawn cleared = Draw(layers, subs, batch, threads, clear);
            const Drawn taken = Draw(layers, subs, batch, threads, 0xF81F, &clear);
            EXPECT_EQ(cleared.pixels, taken.pixels) << batch << " triangles a batch, " << threads << " thread(s)";
            int background = 0;
            for (size_t i = 0; i < cleared.pixels.size(); i += 2)
            {
                background += DekiPixel::Load565(&cleared.pixels[i]) == clear;
            }
            EXPECT_GT(background, kSize * 8) << "the view has room the quads leave";
        }
    }
}
