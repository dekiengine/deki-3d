// Tests that the span loop of a 32-bit framebuffer (ARGB8888) draws exactly
// what the generic loop draws, for every texture kind, shading, cutout and
// depth setting. The generic loop works out a pixel's 8-bit channels the
// same whatever it writes them into, so an RGB888 framebuffer, which keeps
// it, is the reference: each ARGB8888 pixel must be its RGB888 twin, opaque.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../Math3D.h"
#include "../Raster3D.h"
#include "deki-rendering/PixelFormat.h"

using namespace Deki3D;

namespace
{

constexpr int kTexShift = 4;  // 16 x 16 textures
constexpr int kTex = 1 << kTexShift;

// What the background is before a frame draws: no texel or light makes it.
constexpr uint8_t kBackground[3] = { 17, 34, 51 };

// One texture of each kind, with holes where the kind has them: magenta in
// RGB565, index 0 in a palette, alpha below half (127 among them, and 128
// just above) where it carries alpha.
struct Texture
{
    std::vector<uint8_t> texels;
    std::vector<uint16_t> palette;
    Texture3D view;
};

enum class Kind
{
    Flat,
    Palette,
    RGB565,
    RGB565A8,
    RGB888,
    RGBA8888,
    ALPHA8
};
const Kind kKinds[] = { Kind::Flat,   Kind::Palette,  Kind::RGB565, Kind::RGB565A8,
                        Kind::RGB888, Kind::RGBA8888, Kind::ALPHA8 };
const char* const kKindNames[] = { "flat", "palette", "RGB565", "RGB565A8", "RGB888", "RGBA8888", "ALPHA8" };

Texture MakeTexture(Kind kind)
{
    Texture t;
    if (kind == Kind::Flat)
    {
        return t;
    }
    for (int i = 0; i < 16; ++i)
    {
        t.palette.push_back(static_cast<uint16_t>((i * 4099u + 0x1234u) & 0xFFFFu));
    }
    const TexelFormat format = kind == Kind::RGB565A8   ? TexelFormat::RGB565A8
                               : kind == Kind::RGB888   ? TexelFormat::RGB888
                               : kind == Kind::RGBA8888 ? TexelFormat::RGBA8888
                               : kind == Kind::ALPHA8   ? TexelFormat::ALPHA8
                                                        : TexelFormat::RGB565;
    const size_t bytes = kind == Kind::Palette ? 1 : TexelBytes(format);
    t.texels.resize(static_cast<size_t>(kTex) * kTex * bytes);
    for (int i = 0; i < kTex * kTex; ++i)
    {
        const int x = i % kTex, y = i / kTex;
        const bool hole = ((x / 3 + y / 2) % 4) == 0;
        // Every channel value turns up somewhere, the brightest included.
        const uint8_t r = static_cast<uint8_t>(x * 17 + y * 3);
        const uint8_t g = static_cast<uint8_t>(255 - y * 13 - x);
        const uint8_t b = static_cast<uint8_t>((x * y * 7 + 250) & 0xFF);
        const uint8_t a = hole ? static_cast<uint8_t>((x + y) % 2 ? 127 : 0) : static_cast<uint8_t>(128 + x * 8);
        uint16_t v565 = DekiPixel::PackRGB565(r, g, b);
        if (hole)
        {
            v565 = 0xF81F;
        }
        else if (v565 == 0xF81F)
        {
            v565 = 0xF81E;
        }
        uint8_t* d = &t.texels[static_cast<size_t>(i) * bytes];
        switch (kind)
        {
            case Kind::Palette: d[0] = hole ? 0 : static_cast<uint8_t>(1 + (x + y * 3) % 15); break;
            case Kind::RGB565: DekiPixel::Store565(d, v565); break;
            case Kind::RGB565A8:
                DekiPixel::Store565(d, DekiPixel::PackRGB565(r, g, b));
                d[2] = a;
                break;
            case Kind::RGB888:
                d[0] = r;
                d[1] = g;
                d[2] = b;
                break;
            case Kind::RGBA8888:
                d[0] = r;
                d[1] = g;
                d[2] = b;
                d[3] = a;
                break;
            case Kind::ALPHA8: d[0] = a; break;
            case Kind::Flat: break;
        }
    }
    t.view.pixels = t.texels.data();
    t.view.palette = kind == Kind::Palette ? t.palette.data() : nullptr;
    t.view.format = format;
    t.view.width = t.view.height = kTex;
    t.view.widthShift = t.view.heightShift = kTexShift;
    t.view.hasAlpha = kind != Kind::RGB888;
    return t;
}

// Cubes in a grid, turned and at staggered depths so they cut into each
// other, each face its own colour and facing its own way to the light.
struct Cubes
{
    std::vector<VertexPNTC> vertices;
    std::vector<uint16_t> indices;
    Submesh3D sub;
    Mesh3D mesh;
};

Cubes MakeCubes()
{
    Cubes c;
    const Deki::Vector3 normals[6] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
    };
    const Deki::Vector3 us[6] = { { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 1, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    const Deki::Vector3 vs[6] = { { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 } };
    const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    int cube = 0;
    for (int gy = 0; gy < 3; ++gy)
    {
        for (int gx = 0; gx < 4; ++gx, ++cube)
        {
            const Deki::Mat4 place = Mul(Translate(-2.4f + gx * 1.6f, -1.4f + gy * 1.4f, -0.8f * ((gx + gy) % 3)),
                                         Mul(RotateY(0.5f + cube * 0.7f), RotateX(0.3f * cube)));
            for (int f = 0; f < 6; ++f)
            {
                const uint16_t base = static_cast<uint16_t>(c.vertices.size());
                const uint32_t colour = 0xFF000000u | ((40u + cube * 17u) & 0xFF) << 16 |
                                        ((90u + f * 31u) & 0xFF) << 8 | ((200u + cube * f) & 0xFF);
                for (int k = 0; k < 4; ++k)
                {
                    const Deki::Vector3 p = normals[f] + us[f] * corners[k][0] + vs[f] * corners[k][1];
                    float w = 1.0f;
                    VertexPNTC v;
                    v.position = TransformPoint(place, p * 0.75f, w);
                    v.normal = TransformDirection(place, normals[f]);
                    // More than once round the texture, so it wraps.
                    v.u = (corners[k][0] + 1.0f) * 0.8f;
                    v.v = (corners[k][1] + 1.0f) * 0.65f;
                    v.color = colour;
                    c.vertices.push_back(v);
                }
                for (uint16_t i : { 0, 1, 2, 0, 2, 3 })
                {
                    c.indices.push_back(static_cast<uint16_t>(base + i));
                }
            }
        }
    }
    c.sub.indexCount = static_cast<uint32_t>(c.indices.size());
    c.mesh.vertices = c.vertices.data();
    c.mesh.vertexCount = static_cast<uint32_t>(c.vertices.size());
    c.mesh.layout = LayoutPNTC();
    c.mesh.indices = c.indices.data();
    c.mesh.indexCount = static_cast<uint32_t>(c.indices.size());
    c.mesh.submeshes = &c.sub;
    c.mesh.submeshCount = 1;
    c.mesh.boundsMin = Deki::Vector3(-4, -3, -3);
    c.mesh.boundsMax = Deki::Vector3(4, 3, 2);
    return c;
}

struct Drawn
{
    std::vector<uint8_t> pixels;
    RasterStats stats;
};

Drawn Draw(const Cubes& cubes, const Material3D& material, const RasterConfig& config, Deki::ColorFormat format,
           int width, int height)
{
    Drawn drawn;
    drawn.pixels.resize(Deki::FrameBufferBytes(format, width, height));
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
    {
        if (format == Deki::ColorFormat::RGB888)
        {
            DekiPixel::WriteDstPixel<Deki::ColorFormat::RGB888>(drawn.pixels.data(), i, kBackground[0], kBackground[1],
                                                                kBackground[2], 255);
        }
        else
        {
            DekiPixel::WriteDstPixel<Deki::ColorFormat::ARGB8888>(drawn.pixels.data(), i, kBackground[0],
                                                                  kBackground[1], kBackground[2], 255);
        }
    }
    const Deki::Mat4 projection =
        Perspective(1.0f, static_cast<float>(width) / static_cast<float>(height), 0.1f, 50.0f);
    const Deki::Mat4 view = LookAt(Deki::Vector3(0.4f, 0.8f, 6.0f), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 1, 0));
    Raster3D raster;
    raster.BeginFrame(drawn.pixels.data(), width, height, format, config);
    raster.DrawMesh(cubes.mesh, &material, 1, Mul(projection, view), Deki::Mat4::Identity());
    raster.EndFrame();
    drawn.stats = raster.Stats();
    return drawn;
}

// The light, as the span sees it: none, grey no brighter than the colour (the
// one-multiply shading), and per channel, coloured or brighter than the
// colour (clamped).
enum class Lighting
{
    Unlit,
    Grey,
    Coloured,
    Bright
};
const char* const kLightingNames[] = { "unlit", "grey", "coloured", "bright" };

void Light(Lighting lighting, Material3D& material, RasterConfig& config)
{
    material.shading = lighting == Lighting::Unlit ? ShadingModel::Unlit : ShadingModel::VertexLit;
    if (lighting == Lighting::Coloured)
    {
        config.ambient = Deki::Vector3(0.3f, 0.15f, 0.45f);
        config.lights[0].color = Deki::Vector3(1.3f, 0.8f, 0.4f);
    }
    else if (lighting == Lighting::Bright)
    {
        config.ambient = Deki::Vector3(0.6f, 0.6f, 0.6f);
        config.lights[0].color = Deki::Vector3(1.2f, 1.2f, 1.2f);
    }
}

}  // namespace

TEST(RasterSpans, Argb8888DrawsWhatTheGenericLoopDraws)
{
    constexpr int kWidth = 96, kHeight = 72;
    const Cubes cubes = MakeCubes();
    for (int k = 0; k < 7; ++k)
    {
        const Kind kind = kKinds[k];
        const Texture texture = MakeTexture(kind);
        for (int l = 0; l < 4; ++l)
        {
            for (int depth = 0; depth < 4; ++depth)
            {
                uint32_t writtenSolid = 0;
                for (bool cutout : { false, true })
                {
                    SCOPED_TRACE(::testing::Message()
                                 << kKindNames[k] << ", " << kLightingNames[l] << ", depth test " << (depth & 1)
                                 << " write " << (depth >> 1) << ", cutout " << cutout);
                    Material3D material;
                    material.texture = kind == Kind::Flat ? nullptr : &texture.view;
                    material.tint = 0xFFC0E0FFu;
                    material.alphaTest = cutout;
                    RasterConfig config;
                    config.tileSize = 16;
                    config.depthTest = (depth & 1) != 0;
                    config.depthWrite = (depth & 2) != 0;
                    Light(static_cast<Lighting>(l), material, config);

                    const Drawn generic = Draw(cubes, material, config, Deki::ColorFormat::RGB888, kWidth, kHeight);
                    const Drawn tuned = Draw(cubes, material, config, Deki::ColorFormat::ARGB8888, kWidth, kHeight);
                    int mismatches = 0;
                    for (int i = 0; i < kWidth * kHeight; ++i)
                    {
                        const uint8_t* rgb = &generic.pixels[static_cast<size_t>(i) * 3];
                        const uint32_t want = 0xFF000000u | (static_cast<uint32_t>(rgb[0]) << 16) |
                                              (static_cast<uint32_t>(rgb[1]) << 8) | rgb[2];
                        uint32_t got;
                        std::memcpy(&got, &tuned.pixels[static_cast<size_t>(i) * 4], 4);
                        if (got != want && ++mismatches <= 3)
                        {
                            ADD_FAILURE() << "pixel " << i % kWidth << "," << i / kWidth << ": " << std::hex << got
                                          << ", the generic loop " << want;
                        }
                    }
                    EXPECT_EQ(mismatches, 0);
                    EXPECT_EQ(tuned.stats.pixelsWritten, generic.stats.pixelsWritten);
                    EXPECT_GT(tuned.stats.pixelsWritten, 1000u) << "the cubes cover the frame";
                    if (!cutout)
                    {
                        writtenSolid = tuned.stats.pixelsWritten;
                    }
                    else if (kind != Kind::Flat && kind != Kind::RGB888)
                    {
                        EXPECT_LT(tuned.stats.pixelsWritten, writtenSolid) << "the holes are cut out";
                    }
                }
            }
        }
    }
}

// By hand: how long a frame of textured, lit cubes takes to fill into an
// ARGB8888 framebuffer on this machine, for each texture kind an ARGB8888
// target is given and for several tile sizes, on one thread.
TEST(RasterSpans, DISABLED_Argb8888FillSpeed)
{
    const Cubes cubes = MakeCubes();
    const Kind kinds[] = { Kind::Flat, Kind::RGB565, Kind::RGB888, Kind::RGBA8888 };
    for (const int size : { 320, 640 })
    {
        const int width = size, height = size * 3 / 4;
        for (const Kind kind : kinds)
        {
            const Texture texture = MakeTexture(kind);
            for (const int tile : { 32, 64, 128 })
            {
                Material3D material;
                material.texture = kind == Kind::Flat ? nullptr : &texture.view;
                material.alphaTest = kind == Kind::RGBA8888;
                material.shading = ShadingModel::VertexLit;
                RasterConfig config;
                config.tileSize = tile;
                constexpr int kFrames = 200;
                Draw(cubes, material, config, Deki::ColorFormat::ARGB8888, width, height);  // warm
                const auto start = std::chrono::steady_clock::now();
                uint32_t fillMicros = 0;
                for (int f = 0; f < kFrames; ++f)
                {
                    fillMicros +=
                        Draw(cubes, material, config, Deki::ColorFormat::ARGB8888, width, height).stats.fillMicros;
                }
                const double total =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                std::printf("%dx%d %-8s tile %3d: fill %.3f ms (frame %.3f ms)\n", width, height,
                            kKindNames[static_cast<int>(kind)], tile, fillMicros / 1000.0 / kFrames, total / kFrames);
            }
        }
    }
}
