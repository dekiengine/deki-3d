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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "Math3D.h"
#include "Mesh3DBackend.h"
#include "Raster3D.h"
#include "deki-rendering/ShaderAsset.h"
#include "deki-rendering/editor/ShaderUses.h"
#include "editor/MeshShaderUse.h"
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
};

// Looking down at a corner of the cube, so three faces show at three angles
// to the light.
View CornerView(float yaw)
{
    const Deki::Mat4 model = RotateY(yaw);
    const Deki::Mat4 projection = Perspective(1.0f, 1.0f, 0.1f, 50.0f);
    const Deki::Mat4 view = LookAt(Deki::Vector3(2.6f, 2.2f, 3.4f), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 1, 0));
    return { Mul(projection, Mul(view, model)), model };
}

// A quad lying back from the camera, so perspective correction shows.
View SlantedView()
{
    const Deki::Mat4 model = RotateX(-1.0f);
    const Deki::Mat4 projection = Perspective(1.0f, 1.0f, 0.1f, 50.0f);
    const Deki::Mat4 view = LookAt(Deki::Vector3(0, 0, 3.0f), Deki::Vector3(0, 0, 0), Deki::Vector3(0, 1, 0));
    return { Mul(projection, Mul(view, model)), model };
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
    backend.DrawMesh(mesh, &material, 1, v.mvp, v.normalMatrix);
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
    gpu.backend->DrawMesh(cube.mesh, &material, 1, view.mvp, view.normalMatrix);
    gpu.backend->EndFrame();
    drawOnTop(expected);

    // Into the presenter's frame.
    std::vector<uint8_t> layered = background();
    gpu.backend->BeginFrame(layered.data(), kSize, kSize, kFormat, RasterConfig{});
    gpu.backend->DrawMesh(cube.mesh, &material, 1, view.mvp, view.normalMatrix);
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
        gpu.backend->DrawMesh(cube.mesh, m, 1, view.mvp, view.normalMatrix);
        gpu.backend->EndFrame();
        drawOver(expected.data());

        // All of it in the presenter's frame.
        std::vector<uint8_t> buffer = background();
        ASSERT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kSize, kSize, kFormat, kBackground[0], kBackground[1],
                                             kBackground[2]));
        drawUnder(buffer.data());
        gpu.backend->BeginFrame(buffer.data(), kSize, kSize, kFormat, RasterConfig{});
        gpu.backend->DrawMesh(cube.mesh, m, 1, view.mvp, view.normalMatrix);
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
                backend.DrawMesh(cube.mesh, &material, 1, Mul(projection, Mul(view, model)), model);
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
