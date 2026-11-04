// Tests that the GPU backend draws what the software rasteriser draws: the
// same cube in every shading model, the same textured quad with and without
// perspective correction and with holes, into the framebuffer formats a board
// or a desktop uses, over 2D content it must leave alone.
//
// The two are compared pixel by pixel. They are allowed to differ along
// triangle edges (the rasterisers break ties differently) and by the
// software's 32 light levels, and nowhere else. On a machine with no OpenGL
// the tests skip: the backend's whole promise there is to fall back.

// Only where the platform has OpenGL (DEKI_CAPABILITY_OPENGL); a build
// without it, as for a board, has none of the code under test.
#ifdef DEKI_CAPABILITY_OPENGL

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Math3D.h"
#include "Mesh3DBackend.h"
#include "Raster3D.h"
#include "deki-rendering/ShaderAsset.h"
#include "deki-rendering/editor/ShaderUses.h"
#include "MeshAsset.h"
#include "editor/MeshShaderUse.h"
#include "editor/ObjImporter.h"
#include "deki-rendering/GLApi.h"
#include "deki-rendering/GLPresenter.h"
#include "deki-rendering/PixelFormat.h"
#include "deki-rendering/QuadBlit.h"
#include <deki/Time.h>

using namespace Deki3D;

namespace
{

constexpr int kSize = 96;
constexpr uint8_t kBackground[3] = { 10, 20, 30 };

struct Cube
{
    std::vector<VertexPNTC> vertices;
    std::vector<uint16_t> indices;
    Submesh3D sub;
    Mesh3D mesh;
};

// Six faces of a unit cube, counter-clockwise from outside, one colour each.
Cube MakeCube()
{
    Cube c;
    struct Face
    {
        Deki::Vector3 n, u, v;
        uint32_t color;
    };
    const Face faces[6] = {
        { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, 0xFF3050E0u }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 0xFF30E050u },
        { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, 0xFFE05030u }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, 0xFFE0E030u },
        { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, 0xFF30E0E0u }, { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 }, 0xFFE030E0u },
    };
    const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    const float uvs[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (const Face& f : faces)
    {
        const uint16_t base = static_cast<uint16_t>(c.vertices.size());
        for (int k = 0; k < 4; ++k)
        {
            VertexPNTC v;
            v.position = f.n + f.u * corners[k][0] + f.v * corners[k][1];
            v.normal = f.n;
            v.u = uvs[k][0];
            v.v = uvs[k][1];
            v.color = f.color;
            c.vertices.push_back(v);
        }
        for (uint16_t i : { 0, 1, 2, 0, 2, 3 })
        {
            c.indices.push_back(static_cast<uint16_t>(base + i));
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
    c.mesh.boundsMin = Deki::Vector3(-1, -1, -1);
    c.mesh.boundsMax = Deki::Vector3(1, 1, 1);
    return c;
}

struct View
{
    Deki::Mat4 mvp;
    Deki::Mat4 normalMatrix;
    Deki::Mat4 model;
};

// Looking down at a corner of the cube, so three faces show at three angles
// to the light.
View CornerView(float yaw)
{
    const Deki::Mat4 model = RotateY(yaw);
    const Deki::Mat4 projection = Perspective(1.0f, 1.0f, 0.1f, 50.0f);
    const Deki::Mat4 view = LookAt(Deki::Vector3(2.6f, 2.2f, 3.4f), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 1, 0));
    return { Mul(projection, Mul(view, model)), model, model };
}

// A quad lying back from the camera, so perspective correction shows.
View SlantedView()
{
    const Deki::Mat4 model = RotateX(-1.0f);
    const Deki::Mat4 projection = Perspective(1.0f, 1.0f, 0.1f, 50.0f);
    const Deki::Mat4 view = LookAt(Deki::Vector3(0, 0, 3.0f), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 1, 0));
    return { Mul(projection, Mul(view, model)), model, model };
}

std::vector<uint8_t> Background(Deki::ColorFormat format)
{
    std::vector<uint8_t> fb(Deki::FrameBufferBytes(format, kSize, kSize));
    for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
    {
        switch (format)
        {
            case Deki::ColorFormat::RGB565:
                DekiPixel::WriteDstPixel<Deki::ColorFormat::RGB565>(fb.data(), i, kBackground[0], kBackground[1],
                                                                    kBackground[2], 255);
                break;
            default:
                DekiPixel::WriteDstPixel<Deki::ColorFormat::ARGB8888>(fb.data(), i, kBackground[0], kBackground[1],
                                                                      kBackground[2], 255);
                break;
        }
    }
    return fb;
}

void ReadPixel(const std::vector<uint8_t>& fb, Deki::ColorFormat format, size_t i, int rgb[3])
{
    uint8_t r, g, b, a;
    if (format == Deki::ColorFormat::RGB565)
    {
        DekiPixel::ReadDstPixel<Deki::ColorFormat::RGB565>(fb.data(), i, r, g, b, a);
    }
    else
    {
        DekiPixel::ReadDstPixel<Deki::ColorFormat::ARGB8888>(fb.data(), i, r, g, b, a);
    }
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
}

std::vector<uint8_t> Draw(Mesh3DBackend& backend, const Mesh3D& mesh, const Material3D& material, const View& v,
                          Deki::ColorFormat format, const RasterConfig& config)
{
    std::vector<uint8_t> fb = Background(format);
    backend.BeginFrame(fb.data(), kSize, kSize, format, config);
    backend.DrawMesh(mesh, &material, 1, v.mvp, v.normalMatrix, v.model);
    backend.EndFrame();
    return fb;
}

struct Comparison
{
    int softwareDrawn = 0;
    int gpuDrawn = 0;
    int mismatched = 0;  // pixels further apart than the tolerance
};

Comparison Compare(const std::vector<uint8_t>& software, const std::vector<uint8_t>& gpu, Deki::ColorFormat format,
                   int tolerance)
{
    Comparison c;
    int bg[3];
    {
        const std::vector<uint8_t> plain = Background(format);
        ReadPixel(plain, format, 0, bg);
    }
    for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
    {
        int s[3], g[3];
        ReadPixel(software, format, i, s);
        ReadPixel(gpu, format, i, g);
        const bool sDrawn = s[0] != bg[0] || s[1] != bg[1] || s[2] != bg[2];
        const bool gDrawn = g[0] != bg[0] || g[1] != bg[1] || g[2] != bg[2];
        c.softwareDrawn += sDrawn;
        c.gpuDrawn += gDrawn;
        if (std::abs(s[0] - g[0]) > tolerance || std::abs(s[1] - g[1]) > tolerance || std::abs(s[2] - g[2]) > tolerance)
        {
            ++c.mismatched;
        }
    }
    return c;
}

/// The GPU backend, or null where this machine has no OpenGL.
struct Gpu
{
    Mesh3DBackend* backend = CreateGpuBackend();
    ~Gpu()
    {
        if (backend)
        {
            DestroyGpuBackend(backend);
        }
    }
};

#define REQUIRE_GPU(gpu)                                                                                               \
    Gpu gpu;                                                                                                           \
    if (!gpu.backend)                                                                                                \
    {                                                                                                                  \
        GTEST_SKIP() << "no OpenGL on this machine";                                                                   \
    }

// Within this many pixels of each other, as a share of what was drawn: the
// edges, where the two rasterisers break ties differently.
void ExpectAlike(const Comparison& c, double edgeShare, const char* what)
{
    ASSERT_GT(c.softwareDrawn, kSize * kSize / 20) << what << ": the software rasteriser drew too little to compare";
    EXPECT_NEAR(c.gpuDrawn, c.softwareDrawn, c.softwareDrawn * 0.05) << what << ": coverage differs";
    EXPECT_LE(c.mismatched, static_cast<int>(c.softwareDrawn * edgeShare)) << what << ": the pictures differ";
}

}  // namespace

TEST(GLBackend, CubeMatchesSoftwareInEveryShadingModel)
{
    REQUIRE_GPU(gpu);
    Cube cube = MakeCube();
    Raster3D software;
    RasterConfig config;
    const View view = CornerView(0.3f);
    for (ShadingModel shading : { ShadingModel::Unlit, ShadingModel::VertexLit, ShadingModel::Flat })
    {
        for (Deki::ColorFormat format : { Deki::ColorFormat::RGB565, Deki::ColorFormat::ARGB8888 })
        {
            Material3D material;
            material.shading = shading;
            const auto s = Draw(software, cube.mesh, material, view, format, config);
            const auto g = Draw(*gpu.backend, cube.mesh, material, view, format, config);
            // 12: the software's 32 light levels are 8 apart, plus 565 rounding.
            ExpectAlike(Compare(s, g, format, 12), 0.04,
                        shading == ShadingModel::Unlit       ? "unlit"
                        : shading == ShadingModel::VertexLit ? "vertex lit"
                                                             : "flat");
        }
    }
}

TEST(GLBackend, ColouredLightsMatchSoftware)
{
    REQUIRE_GPU(gpu);
    Cube cube = MakeCube();
    Raster3D software;
    RasterConfig config;
    config.ambient = Deki::Vector3(0.15f, 0.1f, 0.2f);
    config.lightCount = 3;
    config.lights[0].kind = Light3D::Kind::Directional;
    config.lights[0].color = Deki::Vector3(0.9f, 0.4f, 0.2f);
    config.lights[0].direction = Deki::Vector3(-0.3f, -0.9f, -0.2f);
    // Far and wide, so a triangle's corners see it alike: the software
    // rasteriser lights a triangle by its first vertex, the GPU in between,
    // which a near point light would tell apart.
    config.lights[1].kind = Light3D::Kind::Point;
    config.lights[1].color = Deki::Vector3(0.2f, 0.6f, 1.0f);
    config.lights[1].position = Deki::Vector3(30.0f, 15.0f, 40.0f);
    config.lights[1].range = 300.0f;
    config.lights[2].kind = Light3D::Kind::Spot;
    config.lights[2].color = Deki::Vector3(0.3f, 0.9f, 0.3f);
    config.lights[2].position = Deki::Vector3(-8.0f, 8.0f, 8.0f);
    config.lights[2].direction = Deki::Vector3(1.0f, -1.0f, -1.0f);
    config.lights[2].range = 60.0f;
    config.lights[2].cosOuter = std::cos(0.6f);
    config.lights[2].cosInner = std::cos(0.4f);
    const View view = CornerView(0.3f);
    for (ShadingModel shading : { ShadingModel::VertexLit, ShadingModel::Flat })
    {
        for (Deki::ColorFormat format : { Deki::ColorFormat::RGB565, Deki::ColorFormat::ARGB8888 })
        {
            Material3D material;
            material.shading = shading;
            const auto s = Draw(software, cube.mesh, material, view, format, config);
            const auto g = Draw(*gpu.backend, cube.mesh, material, view, format, config);
            ExpectAlike(Compare(s, g, format, 16), 0.06, shading == ShadingModel::VertexLit ? "vertex lit" : "flat");
        }
    }
}

// A cube over a floor, the sun from the side and above: the floor is in the
// cube's shadow on the far side and in its light on the near one, and with
// no shadow map the two sides are lit alike.
TEST(GLBackend, ACubeShadowsTheFloor)
{
    REQUIRE_GPU(gpu);
    Cube cube = MakeCube();
    const Deki::Mat4 cubeModel = Deki::Mat4::Identity();
    const Deki::Mat4 floorModel = Compose(0, -1.1f, 0, 0, 0, 0, 6.0f, 0.1f, 6.0f);  // its top at y = -1
    const Deki::Mat4 projection = Perspective(1.2f, 1.0f, 0.1f, 50.0f);
    const Deki::Mat4 view = LookAt(Deki::Vector3(0, 9, 0.01f), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 0, -1));
    const Deki::Mat4 viewProjection = Mul(projection, view);

    RasterConfig config;
    config.ambient = Deki::Vector3(0.1f, 0.1f, 0.1f);
    config.lightCount = 1;
    config.lights[0].kind = Light3D::Kind::Directional;
    config.lights[0].color = Deki::Vector3(1, 1, 1);
    config.lights[0].direction = Deki::Vector3(-1, -1, 0).Normalized();  // from +x, falling toward -x
    config.lights[0].shadow = 0;
    config.shadowCount = 1;
    const Deki::Vector3 dir = config.lights[0].direction;
    const float radius = 9.0f;
    config.shadows[0].viewProjection = Mul(Ortho(-radius, radius, -radius, radius, radius * 0.5f, radius * 3.5f),
                                           LookAt(dir * (-radius * 2.0f), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 1, 0)));
    config.shadows[0].resolution = 512;

    auto drawFrame = [&](bool withShadows)
    {
        Material3D material;
        material.shading = ShadingModel::VertexLit;
        std::vector<uint8_t> fb = Background(Deki::ColorFormat::ARGB8888);
        RasterConfig c = config;
        if (!withShadows)
        {
            c.shadowCount = 0;
            c.lights[0].shadow = -1;
        }
        gpu.backend->BeginFrame(fb.data(), kSize, kSize, Deki::ColorFormat::ARGB8888, c);
        if (withShadows)
        {
            gpu.backend->DrawShadowCaster(0, cube.mesh, &material, 1, cubeModel);
            gpu.backend->DrawShadowCaster(0, cube.mesh, &material, 1, floorModel);
        }
        gpu.backend->DrawMesh(cube.mesh, &material, 1, Mul(viewProjection, cubeModel), cubeModel, cubeModel);
        gpu.backend->DrawMesh(cube.mesh, &material, 1, Mul(viewProjection, floorModel), floorModel, floorModel);
        gpu.backend->EndFrame();
        return fb;
    };
    // The floor's brightness at a world point, seen from above.
    auto brightness = [&](const std::vector<uint8_t>& fb, Deki::Vector3 at)
    {
        float w = 1.0f;
        const Deki::Vector3 clip = TransformPoint(viewProjection, at, w);
        const int x = static_cast<int>((clip.x / w + 1.0f) * 0.5f * kSize);
        const int y = static_cast<int>((1.0f - clip.y / w) * 0.5f * kSize);
        const uint8_t* p = &fb[(static_cast<size_t>(y) * kSize + x) * 4];
        return p[0] + p[1] + p[2];
    };
    ASSERT_TRUE(gpu.backend->DrawsShadows());
    const std::vector<uint8_t> lit = drawFrame(false);
    const std::vector<uint8_t> shadowed = drawFrame(true);
    const Deki::Vector3 farSide(-2.2f, -1.0f, 0.0f), nearSide(2.2f, -1.0f, 0.0f);
    EXPECT_NEAR(brightness(lit, farSide), brightness(lit, nearSide), 6) << "without shadows, alike";
    EXPECT_LT(brightness(shadowed, farSide), brightness(lit, farSide) / 3) << "in the cube's shadow";
    EXPECT_NEAR(brightness(shadowed, nearSide), brightness(lit, nearSide), 6) << "in its light";
}

TEST(GLBackend, TexturedQuadMatchesSoftware)
{
    REQUIRE_GPU(gpu);
    // An 8x8 checker in RGB565, two greys with a magenta hole in one corner.
    std::vector<uint8_t> texels(8 * 8 * 2);
    for (int y = 0; y < 8; ++y)
    {
        for (int x = 0; x < 8; ++x)
        {
            uint16_t v = ((x ^ y) & 1) ? 0xC618 : 0x4208;
            if (x >= 6 && y >= 6)
            {
                v = 0xF81F;
            }
            DekiPixel::Store565(&texels[(y * 8 + x) * 2], v);
        }
    }
    Texture3D tex;
    tex.pixels = texels.data();
    tex.format = TexelFormat::RGB565;
    tex.width = tex.height = 8;
    tex.widthShift = tex.heightShift = 3;
    tex.hasAlpha = true;

    // The cube's front face is a quad with uvs 0..1.
    Cube cube = MakeCube();
    Submesh3D front;
    front.firstIndex = 24;  // +Z face
    front.indexCount = 6;
    cube.mesh.submeshes = &front;

    Raster3D software;
    const View view = SlantedView();
    for (bool perspective : { true, false })
    {
        for (bool alphaTest : { false, true })
        {
            RasterConfig config;
            config.perspectiveCorrect = perspective;
            Material3D material;
            material.texture = &tex;
            material.alphaTest = alphaTest;
            const auto s = Draw(software, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, config);
            const auto g = Draw(*gpu.backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, config);
            // Texel boundaries can land a pixel apart as well as the edges.
            ExpectAlike(Compare(s, g, Deki::ColorFormat::ARGB8888, 8), 0.08,
                        perspective ? (alphaTest ? "perspective, holes" : "perspective")
                                    : (alphaTest ? "affine, holes" : "affine"));
        }
    }
}

// The cube with 70000 unused vertices before its own, so its indices only
// reach them at 32 bits: the same picture as the 16-bit cube, on both.
TEST(GLBackend, VerticesPast65535DrawAsTheSmallMesh)
{
    REQUIRE_GPU(gpu);
    Cube cube = MakeCube();
    const uint32_t pad = 70000;
    std::vector<VertexPNTC> vertices(pad);
    vertices.insert(vertices.end(), cube.vertices.begin(), cube.vertices.end());
    std::vector<uint32_t> indices;
    for (uint16_t i : cube.indices)
    {
        indices.push_back(pad + i);
    }
    Mesh3D wide = cube.mesh;
    wide.vertices = vertices.data();
    wide.vertexCount = static_cast<uint32_t>(vertices.size());
    wide.indices = indices.data();
    wide.indexBytes = 4;

    Raster3D software;
    RasterConfig config;
    Material3D material;
    material.shading = ShadingModel::VertexLit;
    const View view = CornerView(0.3f);
    for (Mesh3DBackend* backend : { static_cast<Mesh3DBackend*>(&software), gpu.backend })
    {
        const auto small = Draw(*backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, config);
        const auto big = Draw(*backend, wide, material, view, Deki::ColorFormat::ARGB8888, config);
        EXPECT_EQ(small, big) << (backend == gpu.backend ? "gpu" : "software");
    }
}

// A bilinear or trilinear texture blends between texels where it is
// magnified; a point one keeps each texel's own colour.
TEST(GLBackend, FilteredTexturesBlendOnTheGpu)
{
    REQUIRE_GPU(gpu);
    std::vector<uint8_t> texels(8 * 8 * 2);
    for (int y = 0; y < 8; ++y)
    {
        for (int x = 0; x < 8; ++x)
        {
            DekiPixel::Store565(&texels[(y * 8 + x) * 2], ((x ^ y) & 1) ? 0xFFFF : 0x0000);
        }
    }
    Texture3D tex;
    tex.pixels = texels.data();
    tex.format = TexelFormat::RGB565;
    tex.width = tex.height = 8;
    tex.widthShift = tex.heightShift = 3;

    Cube cube = MakeCube();
    Submesh3D front;
    front.firstIndex = 24;  // +Z face
    front.indexCount = 6;
    cube.mesh.submeshes = &front;
    const View view = SlantedView();
    RasterConfig config;

    auto greys = [&](Deki::TextureFilter filter)
    {
        tex.filter = filter;
        Material3D material;
        material.texture = &tex;
        const auto fb = Draw(*gpu.backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, config);
        int between = 0;
        for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
        {
            int rgb[3];
            ReadPixel(fb, Deki::ColorFormat::ARGB8888, i, rgb);
            const bool background = rgb[0] == kBackground[0] && rgb[1] == kBackground[1] && rgb[2] == kBackground[2];
            between += !background && rgb[0] > 40 && rgb[0] < 215 ? 1 : 0;
        }
        return between;
    };
    const int point = greys(Deki::TextureFilter::Point);
    const int bilinear = greys(Deki::TextureFilter::Bilinear);
    const int trilinear = greys(Deki::TextureFilter::Trilinear);
    EXPECT_LT(point, kSize * kSize / 50) << "point sampling draws black and white";
    EXPECT_GT(bilinear, kSize * kSize / 10) << "bilinear sampling draws the greys between";
    EXPECT_GT(trilinear, kSize * kSize / 10) << "trilinear sampling draws the greys between";
}

TEST(GLBackend, LeavesTheBackgroundAlone)
{
    REQUIRE_GPU(gpu);
    Cube cube = MakeCube();
    Material3D material;
    const auto fb = Draw(*gpu.backend, cube.mesh, material, CornerView(0.0f), Deki::ColorFormat::RGB565, {});
    int corner[3];
    for (size_t i : { size_t(0), size_t(kSize - 1), size_t(kSize) * (kSize - 1), size_t(kSize) * kSize - 1 })
    {
        ReadPixel(fb, Deki::ColorFormat::RGB565, i, corner);
        const auto bg = Background(Deki::ColorFormat::RGB565);
        int want[3];
        ReadPixel(bg, Deki::ColorFormat::RGB565, 0, want);
        EXPECT_EQ(corner[0], want[0]);
        EXPECT_EQ(corner[1], want[1]);
        EXPECT_EQ(corner[2], want[2]);
    }
}

// A mesh is uploaded once per revision: the same revision draws the upload
// it has, a new one uploads again.
TEST(GLBackend, UploadsFollowTheRevision)
{
    REQUIRE_GPU(gpu);
    Cube cube = MakeCube();
    cube.mesh.revision = NextRevision();
    Material3D material;
    const View view = CornerView(0.3f);
    const auto before = Draw(*gpu.backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, {});

    for (VertexPNTC& v : cube.vertices)
    {
        v.color = 0xFF00FF00u;  // all green
    }
    const auto stale = Draw(*gpu.backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, {});
    EXPECT_EQ(stale, before) << "the same revision redraws what was uploaded";

    cube.mesh.revision = NextRevision();
    const auto fresh = Draw(*gpu.backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, {});
    int centre[3];
    ReadPixel(fresh, Deki::ColorFormat::ARGB8888, static_cast<size_t>(kSize / 2) * kSize + kSize / 2, centre);
    EXPECT_EQ(centre[0], 0);
    EXPECT_EQ(centre[1], 255);
    EXPECT_EQ(centre[2], 0);
}

namespace
{

/// HLSL `text` compiled by the editor's compiler, as a build ships it.
DekiRendering::ShaderAsset Shader(const char* text)
{
    DekiRendering::ShaderAsset shader;
    std::string errors;
    EXPECT_TRUE(DekiRendering::CompileShaderText(text, "test.shader", shader, errors)) << errors;
    return shader;
}

// How many pixels differ from the background, and the colour at the centre.
int Covered(const std::vector<uint8_t>& fb)
{
    return Compare(fb, Background(Deki::ColorFormat::ARGB8888), Deki::ColorFormat::ARGB8888, 0).mismatched;
}

}  // namespace

// A shader's Shade() decides the colour, reading what the component gives it.
TEST(GLBackend, AShaderColoursTheMesh)
{
    REQUIRE_GPU(gpu);
    const DekiRendering::ShaderAsset shader = Shader(R"(
float4 Shade(Fragment f)
{
    return float4(uColor.rgb * uParams.x, 1.0);
}
)");
    Cube cube = MakeCube();
    Material3D material;
    material.shading = ShadingModel::VertexLit;
    material.shader = &shader;
    material.shaderColor = 0xFF00FF00u;  // green
    material.shaderParams = Deki::Vector3(0.5f, 0.0f, 0.0f);
    const auto fb = Draw(*gpu.backend, cube.mesh, material, CornerView(0.3f), Deki::ColorFormat::ARGB8888, {});
    int centre[3];
    ReadPixel(fb, Deki::ColorFormat::ARGB8888, static_cast<size_t>(kSize / 2) * kSize + kSize / 2, centre);
    EXPECT_EQ(centre[0], 0);
    EXPECT_NEAR(centre[1], 128, 1);
    EXPECT_EQ(centre[2], 0);
}

// A frame drawn with a shader that reads the time says so, so a view that
// renders only on change (the editor's Scene View) keeps it moving; one
// without does not.
TEST(GLBackend, AShaderReadingTheTimeMarksTheFrame)
{
    REQUIRE_GPU(gpu);
    const DekiRendering::ShaderAsset pulse =
        Shader("float4 Shade(Fragment f) { return f.base * (0.5 + 0.5 * sin(uTime)); }\n");
    const DekiRendering::ShaderAsset still = Shader("float4 Shade(Fragment f) { return f.base; }\n");
    Cube cube = MakeCube();
    Material3D material;
    const View view = CornerView(0.3f);

    Deki::Time::TakeTimeDrivenFrame();
    Draw(*gpu.backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, {});
    EXPECT_FALSE(Deki::Time::TakeTimeDrivenFrame());

    material.shader = &still;
    Draw(*gpu.backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, {});
    EXPECT_FALSE(Deki::Time::TakeTimeDrivenFrame());

    material.shader = &pulse;
    Draw(*gpu.backend, cube.mesh, material, view, Deki::ColorFormat::ARGB8888, {});
    EXPECT_TRUE(Deki::Time::TakeTimeDrivenFrame());
}

// Shade() is given what the shading model would draw, so a shader can build
// on it rather than redo it.
TEST(GLBackend, AShaderStartsFromTheShadingModel)
{
    REQUIRE_GPU(gpu);
    const DekiRendering::ShaderAsset shader = Shader("float4 Shade(Fragment f) { return f.base; }\n");
    Cube cube = MakeCube();
    for (ShadingModel model : { ShadingModel::Unlit, ShadingModel::VertexLit, ShadingModel::Flat })
    {
        SCOPED_TRACE(static_cast<int>(model));
        Material3D plain;
        plain.shading = model;
        Material3D shaded = plain;
        shaded.shader = &shader;
        const View view = CornerView(0.3f);
        const auto a = Draw(*gpu.backend, cube.mesh, plain, view, Deki::ColorFormat::ARGB8888, {});
        const auto b = Draw(*gpu.backend, cube.mesh, shaded, view, Deki::ColorFormat::ARGB8888, {});
        EXPECT_EQ(a, b);
    }
}

// The built-in program is the mesh use's HLSL with no hooks, compiled by the
// editor's compiler into GLMesh3DBuiltinShader.inl. This fails when the .inl
// is not what the HLSL compiles to now; DEKI_UPDATE_GENERATED=1 rewrites it.
TEST(GLBackend, TheBuiltInShaderIsWhatTheMeshUseCompilesTo)
{
    DekiRendering::ShaderProgram program;
    std::string errors;
    ASSERT_TRUE(DekiRendering::CompileShaderUse(Deki3D::MeshShaderUse(), "", "built-in", {}, program, errors))
        << errors;
    auto literal = [](const char* name, const std::string& text)
    { return "const char* const " + std::string(name) + " = R\"deki(" + text + ")deki\";\n"; };
    const std::string generated =
        "// The built-in mesh program: editor/MeshShaderUse.cpp with no hooks, as the\n"
        "// editor's compiler writes it. Generated by tests/GLBackendTests.cpp\n"
        "// (TheBuiltInShaderIsWhatTheMeshUseCompilesTo); do not edit.\n\n" +
        literal("kBuiltinMeshVertexES", program.es.vertex) + "\n" +
        literal("kBuiltinMeshFragmentES", program.es.fragment) + "\n" +
        literal("kBuiltinMeshVertexGL", program.gl.vertex) + "\n" +
        literal("kBuiltinMeshFragmentGL", program.gl.fragment);

    const std::string path = std::string(DEKI_3D_SOURCE_DIR) + "/GLMesh3DBuiltinShader.inl";
    std::string current;
    if (FILE* in = std::fopen(path.c_str(), "rb"))
    {
        char chunk[4096];
        for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), in)) > 0;)
        {
            current.append(chunk, n);
        }
        std::fclose(in);
    }
    // A Windows checkout may have given it CRLF line ends.
    current.erase(std::remove(current.begin(), current.end(), '\r'), current.end());
    if (current != generated && std::getenv("DEKI_UPDATE_GENERATED"))
    {
        FILE* out = std::fopen(path.c_str(), "wb");
        ASSERT_NE(out, nullptr) << path;
        std::fwrite(generated.data(), 1, generated.size(), out);
        std::fclose(out);
        current = generated;
    }
    EXPECT_EQ(current, generated) << path << " is stale: run the deki-3d tests with DEKI_UPDATE_GENERATED=1";
}

TEST(GLBackend, AShaderCanMoveTheVertices)
{
    REQUIRE_GPU(gpu);
    const DekiRendering::ShaderAsset half = Shader(R"(
// Shared by both stages.
static const float kScale = 0.5;
float3 Displace(float3 position, float3 normal)
{
    return position * kScale;
}
)");
    Cube cube = MakeCube();
    Material3D plain;
    Material3D shrunk;
    shrunk.shader = &half;
    const View view = CornerView(0.3f);
    const int whole = Covered(Draw(*gpu.backend, cube.mesh, plain, view, Deki::ColorFormat::ARGB8888, {}));
    const int part = Covered(Draw(*gpu.backend, cube.mesh, shrunk, view, Deki::ColorFormat::ARGB8888, {}));
    ASSERT_GT(whole, 0);
    // Half the size, about a quarter of the area (perspective bends it a little).
    EXPECT_GT(part, whole / 6);
    EXPECT_LT(part, whole / 3);
}

// A shader that does not compile is an error in the editor, naming the file
// and line, and never reaches a device.
TEST(GLBackend, AShaderThatDoesNotCompileSaysWhereAndWhy)
{
    DekiRendering::ShaderAsset shader;
    std::string errors;
    EXPECT_FALSE(DekiRendering::CompileShaderText("float4 Shade(Fragment f)\n{\n    return nonsense;\n}\n",
                                                  "broken.shader", shader, errors));
    EXPECT_NE(errors.find("broken.shader:3:"), std::string::npos) << errors;
    EXPECT_NE(errors.find("nonsense"), std::string::npos) << errors;
}

// A shader with no mesh hooks (an effect's, say) draws a mesh as if there
// were none, and says so in the log.
TEST(GLBackend, AShaderWithoutMeshHooksDrawsAsNone)
{
    REQUIRE_GPU(gpu);
    const DekiRendering::ShaderAsset effect = Shader("float4 Effect(float2 uv) { return Frame(uv); }\n");
    ASSERT_EQ(effect.Program("mesh", true), nullptr);
    Cube cube = MakeCube();
    Material3D plain;
    plain.shading = ShadingModel::VertexLit;
    Material3D shaded = plain;
    shaded.shader = &effect;
    const View view = CornerView(0.3f);
    const auto a = Draw(*gpu.backend, cube.mesh, plain, view, Deki::ColorFormat::ARGB8888, {});
    const auto b = Draw(*gpu.backend, cube.mesh, shaded, view, Deki::ColorFormat::ARGB8888, {});
    EXPECT_EQ(a, b);
}

// Where a presenter builds the frame on the GPU, the meshes draw into it
// between layers of 2D. The picture must be the one reading back gives: 2D
// below, the meshes, then 2D drawn after them on top.
TEST(GLBackend, DrawingIntoThePresenterMatchesReadingBack)
{
    REQUIRE_GPU(gpu);
    DekiGL::GLPresenter::Window window{ [](void*) {},
                                        [](void*, int* w, int* h)
                                        {
                                            *w = kSize;
                                            *h = kSize;
                                        },
                                        nullptr };
    std::unique_ptr<DekiGL::GLPresenter> presenter = DekiGL::GLPresenter::Create(DekiGL::SharedContext(), window);
    ASSERT_NE(presenter, nullptr);

    constexpr Deki::ColorFormat kFormat = Deki::ColorFormat::RGB565A8;
    Cube cube = MakeCube();
    Material3D material;
    material.shading = ShadingModel::Flat;
    const View view = CornerView(0.3f);
    auto background = [&]()
    {
        std::vector<uint8_t> fb(Deki::FrameBufferBytes(kFormat, kSize, kSize));
        for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
        {
            DekiPixel::WriteDstPixel<kFormat>(fb.data(), i, kBackground[0], kBackground[1], kBackground[2], 255);
        }
        return fb;
    };
    // 2D after the meshes: an opaque bar across the middle, over the cube.
    auto drawOnTop = [&](std::vector<uint8_t>& fb)
    {
        for (int x = 10; x < kSize - 10; ++x)
        {
            for (int y = kSize / 2 - 2; y < kSize / 2 + 2; ++y)
            {
                DekiPixel::WriteDstPixel<kFormat>(fb.data(), static_cast<size_t>(y) * kSize + x, 250, 250, 0, 255);
            }
        }
    };

    // Reading back.
    RasterConfig readBack;
    readBack.composeOnGpu = false;
    std::vector<uint8_t> expected = background();
    gpu.backend->BeginFrame(expected.data(), kSize, kSize, kFormat, readBack);
    gpu.backend->DrawMesh(cube.mesh, &material, 1, view.mvp, view.normalMatrix, view.model);
    gpu.backend->EndFrame();
    drawOnTop(expected);

    // Into the presenter's frame.
    std::vector<uint8_t> layered = background();
    gpu.backend->BeginFrame(layered.data(), kSize, kSize, kFormat, {});
    gpu.backend->DrawMesh(cube.mesh, &material, 1, view.mvp, view.normalMatrix, view.model);
    gpu.backend->EndFrame();
    uint8_t r, g, b, a;
    DekiPixel::ReadDstPixel<kFormat>(layered.data(), 0, r, g, b, a);
    EXPECT_EQ(a, 0) << "after the meshes the buffer starts over, transparent";
    drawOnTop(layered);
    presenter->PushLayer(layered.data(), kSize, kSize, kFormat);
    std::vector<uint8_t> frame;
    ASSERT_TRUE(presenter->ReadFrame(frame));

    int mismatched = 0, drawn = 0;
    for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
    {
        DekiPixel::ReadDstPixel<kFormat>(expected.data(), i, r, g, b, a);
        const uint8_t* p = &frame[i * 4];
        drawn += r != kBackground[0] || g != kBackground[1] || b != kBackground[2];
        // 8: the readback goes through 565, the GPU frame does not.
        if (std::abs(p[0] - r) > 8 || std::abs(p[1] - g) > 8 || std::abs(p[2] - b) > 8)
        {
            ++mismatched;
        }
    }
    EXPECT_GT(drawn, kSize * kSize / 10);
    EXPECT_EQ(mismatched, 0);
}

// With the 2D drawn by the presenter too (QuadBlit's sink), the meshes still
// sort between it: 2D drawn before them below, 2D drawn after on top. The
// picture must be the one the CPU's 2D and a read-back mesh make.
TEST(GLBackend, MeshesSortBetween2DDrawnOnTheGpu)
{
    REQUIRE_GPU(gpu);
    DekiGL::GLPresenter::Window window{ [](void*) {},
                                        [](void*, int* w, int* h)
                                        {
                                            *w = kSize;
                                            *h = kSize;
                                        },
                                        nullptr };
    std::unique_ptr<DekiGL::GLPresenter> presenter = DekiGL::GLPresenter::Create(DekiGL::SharedContext(), window);
    ASSERT_NE(presenter, nullptr);
    ASSERT_TRUE(presenter->Draws2D());

    constexpr Deki::ColorFormat kFormat = Deki::ColorFormat::RGB565A8;
    Cube cube = MakeCube();
    Material3D material;
    material.shading = ShadingModel::Flat;
    const View view = CornerView(0.3f);

    // Below the meshes, a checker over the whole frame; above, a bar across
    // the middle, half see-through.
    std::vector<uint8_t> checker(8 * 8 * 2);
    for (int i = 0; i < 64; ++i)
    {
        const bool dark = ((i % 8) + (i / 8)) % 2 == 0;
        DekiPixel::Store565(&checker[i * 2],
                            dark ? DekiPixel::PackRGB565(40, 40, 80) : DekiPixel::PackRGB565(80, 120, 160));
    }
    const QuadBlit::Source under = QuadBlit::MakeSource(checker.data(), 8, 8, QuadBlit::PixelLayout::RGB565());
    uint8_t barPixels[2 * 3];
    DekiPixel::Store565(&barPixels[0], DekiPixel::PackRGB565(248, 248, 0));
    barPixels[2] = 255;
    DekiPixel::Store565(&barPixels[3], DekiPixel::PackRGB565(248, 0, 248));
    barPixels[5] = 128;
    const QuadBlit::Source over = QuadBlit::MakeSource(barPixels, 2, 1, QuadBlit::PixelLayout::RGB565A8());
    auto background = [&]()
    {
        std::vector<uint8_t> fb(Deki::FrameBufferBytes(kFormat, kSize, kSize));
        for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
        {
            DekiPixel::WriteDstPixel<kFormat>(fb.data(), i, kBackground[0], kBackground[1], kBackground[2], 255);
        }
        return fb;
    };
    auto drawUnder = [&](uint8_t* fb)
    { QuadBlit::Blit(under, fb, kSize, kSize, kFormat, 0, 0, kSize / 8.0f, kSize / 8.0f, 0, 0, 0); };
    auto drawOver = [&](uint8_t* fb)
    { QuadBlit::BlitScaled(over, fb, kSize, kSize, kFormat, 6, kSize / 2 - 3, kSize - 12, 6); };

    // A mesh with a shader of its own too, which moves its faces and shades
    // it: the 2D's GL state must not reach it. Inward, as the reference reads
    // back only the mesh's bounds.
    const DekiRendering::ShaderAsset swell = Shader(R"(
float3 Displace(float3 position, float3 normal)
{
    return position + normal * uParams.x;
}
float4 Shade(Fragment f)
{
    return float4(lerp(f.base.rgb, uColor.rgb * f.light, 0.6), 1.0);
}
)");
    Material3D shaded = material;
    shaded.shader = &swell;
    shaded.shaderParams = Deki::Vector3(-0.2f, 0.0f, 0.0f);
    shaded.shaderColor = 0xFF28C8FFu;
    for (const Material3D* m : { &material, &shaded })
    {
        SCOPED_TRACE(m->shader ? "a shader" : "built in");
        // On the CPU, the meshes read back between the two.
        RasterConfig readBack;
        readBack.composeOnGpu = false;
        std::vector<uint8_t> expected = background();
        drawUnder(expected.data());
        gpu.backend->BeginFrame(expected.data(), kSize, kSize, kFormat, readBack);
        gpu.backend->DrawMesh(cube.mesh, m, 1, view.mvp, view.normalMatrix, view.model);
        gpu.backend->EndFrame();
        drawOver(expected.data());

        // All of it in the presenter's frame.
        std::vector<uint8_t> buffer = background();
        ASSERT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kSize, kSize, kFormat, kBackground[0], kBackground[1],
                                             kBackground[2]));
        drawUnder(buffer.data());
        gpu.backend->BeginFrame(buffer.data(), kSize, kSize, kFormat, {});
        gpu.backend->DrawMesh(cube.mesh, m, 1, view.mvp, view.normalMatrix, view.model);
        gpu.backend->EndFrame();
        drawOver(buffer.data());
        QuadBlit::EndSinkFrame();
        presenter->PushLayer(buffer.data(), kSize, kSize, kFormat);
        std::vector<uint8_t> frame;
        ASSERT_TRUE(presenter->ReadFrame(frame));

        std::vector<uint8_t> noMesh = background();
        drawUnder(noMesh.data());
        drawOver(noMesh.data());
        int mismatched = 0, meshPixels = 0;
        for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
        {
            uint8_t r, g, b, a, r0, g0, b0;
            DekiPixel::ReadDstPixel<kFormat>(noMesh.data(), i, r0, g0, b0, a);
            DekiPixel::ReadDstPixel<kFormat>(expected.data(), i, r, g, b, a);
            meshPixels += r != r0 || g != g0 || b != b0;
            const uint8_t* p = &frame[i * 4];
            // 8: the CPU's 2D and the readback go through 565, the GPU frame does not.
            if (std::abs(p[0] - r) > 8 || std::abs(p[1] - g) > 8 || std::abs(p[2] - b) > 8)
            {
                ++mismatched;
            }
        }
        EXPECT_GT(meshPixels, kSize * kSize / 10) << "the cube shows over the checker";
        EXPECT_EQ(mismatched, 0);
    }
}

// Not a check: how long a frame of many lit cubes takes each way at a
// handheld's resolution. Run it with --gtest_also_run_disabled_tests.
TEST(GLBackend, DISABLED_BenchmarkAgainstSoftware)
{
    REQUIRE_GPU(gpu);
    constexpr int kWidth = 640, kHeight = 480, kCubes = 400, kFrames = 30;
    Cube cube = MakeCube();
    cube.mesh.revision = NextRevision();
    Material3D material;
    material.shading = ShadingModel::VertexLit;
    const Deki::Mat4 projection = Perspective(1.0f, static_cast<float>(kWidth) / kHeight, 0.1f, 200.0f);
    const Deki::Mat4 view = LookAt(Deki::Vector3(0, 6, 30), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 1, 0));
    std::vector<uint8_t> fb(Deki::FrameBufferBytes(Deki::ColorFormat::RGB565, kWidth, kHeight));

    auto time = [&](Mesh3DBackend& backend)
    {
        RasterConfig config;
        const auto start = std::chrono::steady_clock::now();
        for (int f = 0; f < kFrames; ++f)
        {
            backend.BeginFrame(fb.data(), kWidth, kHeight, Deki::ColorFormat::RGB565, config);
            for (int i = 0; i < kCubes; ++i)
            {
                const float x = static_cast<float>(i % 20) * 2.6f - 25.0f;
                const float z = static_cast<float>(i / 20) * -2.6f + 10.0f;
                const Deki::Mat4 model = Mul(Translate(x, 0, z), RotateY(0.05f * f + i));
                backend.DrawMesh(cube.mesh, &material, 1, Mul(projection, Mul(view, model)), model, model);
            }
            backend.EndFrame();
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
    };
    Raster3D software;
    const double sw = time(software);
    const double gl = time(*gpu.backend);
    std::printf("%d cubes at %dx%d: software %.2f ms/frame, GPU %.2f ms/frame\n", kCubes, kWidth, kHeight, sw, gl);
}

#endif  // DEKI_CAPABILITY_OPENGL

// A pillar off the centre of a big floor, seen from above, the sun falling
// toward +x: its shadow lies on the floor on its +x side, as long as it is
// tall, and nowhere else. Off centre and at scene scale on purpose: the cube
// above sits in the middle of its shadow map, where a mirrored or shifted
// lookup would land on the right texels anyway.
TEST(GLBackend, AnOffCentrePillarShadowsItsFarSide)
{
    REQUIRE_GPU(gpu);
    std::string obj;
    int vertices = 0;
    auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1)
    {
        const int base = vertices;
        vertices += 8;
        for (float x : { x0, x1 })
        {
            for (float y : { y0, y1 })
            {
                for (float z : { z0, z1 })
                {
                    obj += "v " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z) + "\n";
                }
            }
        }
        auto i = [&](int x, int y, int z) { return std::to_string(base + 1 + x * 4 + y * 2 + z); };
        const int q[6][4][3] = { { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 } },
                                 { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } },
                                 { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } },
                                 { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
                                 { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } },
                                 { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } } };
        for (const auto& face : q)
        {
            obj += "f";
            for (const auto& c : face)
            {
                obj += " " + i(c[0], c[1], c[2]);
            }
            obj += "\n";
        }
    };
    box(-2000, -20, -2000, 2000, 0, 2000);  // the floor, its top at y = 0
    box(700, 0, 300, 900, 1000, 500);       // the pillar
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(obj, "", {}, blob, error)) << error;
    MeshAsset asset;
    ASSERT_TRUE(asset.LoadFromMemory(blob.data(), blob.size()));
    const Mesh3D& mesh = asset.View();

    // Straight down from above; on screen +x is right and +z is down.
    const Deki::Mat4 viewProjection = Mul(Perspective(1.2f, 1.0f, 100.0f, 10000.0f),
                                          LookAt(Deki::Vector3(0, 4200, 0), Deki::Vector3(0, 0, 0),
                                                 Deki::Vector3(0, 0, -1)));
    RasterConfig config;
    config.ambient = Deki::Vector3(0.1f, 0.1f, 0.1f);
    config.lightCount = 1;
    config.lights[0].kind = Light3D::Kind::Directional;
    config.lights[0].color = Deki::Vector3(1, 1, 1);
    config.lights[0].direction = Deki::Vector3(1, -1, 0).Normalized();
    config.lights[0].shadow = 0;
    config.shadowCount = 1;
    const Deki::Vector3 centre = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
    const float radius = (mesh.boundsMax - mesh.boundsMin).Length() * 0.5f;
    const Deki::Vector3 dir = config.lights[0].direction;
    config.shadows[0].viewProjection = Mul(Ortho(-radius, radius, -radius, radius, radius * 0.5f, radius * 3.5f),
                                           LookAt(centre - dir * (radius * 2.0f), centre, Deki::Vector3(0, 1, 0)));
    config.shadows[0].resolution = 1024;

    Material3D material;
    material.shading = ShadingModel::VertexLit;
    std::vector<uint8_t> fb = Background(Deki::ColorFormat::ARGB8888);
    gpu.backend->BeginFrame(fb.data(), kSize, kSize, Deki::ColorFormat::ARGB8888, config);
    gpu.backend->DrawShadowCaster(0, mesh, &material, 1, Deki::Mat4::Identity());
    gpu.backend->DrawMesh(mesh, &material, 1, viewProjection, Deki::Mat4::Identity(), Deki::Mat4::Identity());
    gpu.backend->EndFrame();

    // The floor point at world (x, z), on screen.
    auto brightness = [&](float x, float z)
    {
        float w = 1.0f;
        const Deki::Vector3 ndc = TransformPoint(viewProjection, Deki::Vector3(x, 0, z), w);
        const int px = static_cast<int>((ndc.x / w * 0.5f + 0.5f) * kSize);
        const int py = static_cast<int>((0.5f - ndc.y / w * 0.5f) * kSize);
        int rgb[3];
        ReadPixel(fb, Deki::ColorFormat::ARGB8888, static_cast<size_t>(py) * kSize + px, rgb);
        return rgb[0] + rgb[1] + rgb[2];
    };
    const int lit = brightness(-800, 400);  // across the floor from the pillar
    EXPECT_LT(brightness(1400, 400), lit / 3) << "the pillar's shadow, beyond it along the light";
    EXPECT_GT(brightness(400, 400), lit * 2 / 3) << "lit before the pillar";
    EXPECT_GT(brightness(1400, -800), lit * 2 / 3) << "lit beside the shadow";
    EXPECT_GT(brightness(-1400, -400), lit * 2 / 3) << "lit where a mirrored shadow would land";
}

// A real model, named by DEKI_TEST_OBJ, drawn from DEKI_TEST_EYE ("x y z
// yaw pitch", degrees) under a sun at DEKI_TEST_SUN ("yaw pitch") with and
// without its shadows, the shadow map fitted as Mesh3DPass fits it. Writes
// shadows.ppm and plain.ppm into DEKI_TEST_OUT. Run by hand with
// --gtest_also_run_disabled_tests.
TEST(GLBackend, DISABLED_AModelUnderTheSun)
{
    REQUIRE_GPU(gpu);
    const char* path = std::getenv("DEKI_TEST_OBJ");
    const char* out = std::getenv("DEKI_TEST_OUT");
    if (!path || !out)
    {
        GTEST_SKIP() << "set DEKI_TEST_OBJ and DEKI_TEST_OUT";
    }
    FILE* f = std::fopen(path, "rb");
    ASSERT_NE(f, nullptr);
    std::string text;
    char chunk[65536];
    for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;)
    {
        text.append(chunk, n);
    }
    std::fclose(f);
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(text, "", {}, blob, error)) << error;
    MeshAsset asset;
    ASSERT_TRUE(asset.LoadFromMemory(blob.data(), blob.size()));
    const Mesh3D& mesh = asset.View();

    float ex = 1150, ey = 180, ez = -40, eyaw = 90, epitch = 4.6f, syaw = 20, spitch = 62;
    if (const char* e = std::getenv("DEKI_TEST_EYE"))
    {
        std::sscanf(e, "%f %f %f %f %f", &ex, &ey, &ez, &eyaw, &epitch);
    }
    if (const char* s = std::getenv("DEKI_TEST_SUN"))
    {
        std::sscanf(s, "%f %f", &syaw, &spitch);
    }
    const float d2r = 3.14159265f / 180.0f;
    const int w = 384, h = 216;
    const Deki::Mat4 basis = Mul(RotateY(eyaw * d2r), RotateX(epitch * d2r));
    const Deki::Vector3 eye(ex, ey, ez);
    const Deki::Vector3 forward = TransformDirection(basis, Deki::Vector3(0, 0, -1));
    const Deki::Vector3 up = TransformDirection(basis, Deki::Vector3(0, 1, 0));
    const Deki::Mat4 viewProjection =
        Mul(Perspective(65.0f * d2r, static_cast<float>(w) / h, 5.0f, 6000.0f), LookAt(eye, eye + forward, up));

    RasterConfig config;
    config.ambient = Deki::Vector3(0.18f, 0.2f, 0.25f);
    config.lightCount = 1;
    config.lights[0].kind = Light3D::Kind::Directional;
    config.lights[0].color = Deki::Vector3(1, 0.95f, 0.85f);
    config.lights[0].direction =
        Deki::Vector3(std::sin(syaw * d2r) * std::cos(spitch * d2r), -std::sin(spitch * d2r),
                      -std::cos(syaw * d2r) * std::cos(spitch * d2r));
    config.lights[0].shadow = 0;
    config.shadowCount = 1;
    const Deki::Vector3 centre = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
    const float radius = (mesh.boundsMax - mesh.boundsMin).Length() * 0.5f;
    const Deki::Vector3 dir = config.lights[0].direction;
    config.shadows[0].viewProjection = Mul(Ortho(-radius, radius, -radius, radius, radius * 0.5f, radius * 3.5f),
                                           LookAt(centre - dir * (radius * 2.0f), centre, Deki::Vector3(0, 1, 0)));
    config.shadows[0].resolution = 2048;
    config.shadows[0].bias = 0.002f;
    if (const char* b = std::getenv("DEKI_TEST_BIAS"))
    {
        config.shadows[0].bias = static_cast<float>(std::atof(b));
    }

    std::vector<Material3D> materials(asset.Materials(), asset.Materials() + asset.MaterialCount());
    for (Material3D& m : materials)
    {
        m.shading = ShadingModel::VertexLit;
        m.doubleSided = true;
    }
    for (bool withShadows : { true, false })
    {
        RasterConfig c = config;
        if (!withShadows)
        {
            c.shadowCount = 0;
            c.lights[0].shadow = -1;
        }
        std::vector<uint8_t> fb(Deki::FrameBufferBytes(Deki::ColorFormat::ARGB8888, w, h), 0);
        gpu.backend->BeginFrame(fb.data(), w, h, Deki::ColorFormat::ARGB8888, c);
        if (withShadows)
        {
            gpu.backend->DrawShadowCaster(0, mesh, materials.data(), static_cast<uint16_t>(materials.size()),
                                          Deki::Mat4::Identity());
        }
        gpu.backend->DrawMesh(mesh, materials.data(), static_cast<uint16_t>(materials.size()), viewProjection,
                              Deki::Mat4::Identity(), Deki::Mat4::Identity());
        gpu.backend->EndFrame();
        const std::string name = std::string(out) + (withShadows ? "/shadows.ppm" : "/plain.ppm");
        FILE* ppm = std::fopen(name.c_str(), "wb");
        ASSERT_NE(ppm, nullptr);
        std::fprintf(ppm, "P6 %d %d 255\n", w, h);
        for (int i = 0; i < w * h; ++i)
        {
            int rgb[3];
            uint8_t r, g, b, a;
            DekiPixel::ReadDstPixel<Deki::ColorFormat::ARGB8888>(fb.data(), static_cast<size_t>(i), r, g, b, a);
            rgb[0] = r;
            rgb[1] = g;
            rgb[2] = b;
            for (int k = 0; k < 3; ++k)
            {
                std::fputc(rgb[k], ppm);
            }
        }
        std::fclose(ppm);
    }
}
