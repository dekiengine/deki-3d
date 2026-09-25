/**
 * @file Raster3DBench.cpp
 * @brief Correctness checks and timings for the software rasteriser.
 *
 * The timings are the point: everything downstream of the rasteriser was
 * planned against an assumed cost per pixel, and this replaces the assumption
 * with a measurement. It links nothing but the rasteriser, so it can run long
 * before the package has a component, an asset or a scene.
 *
 * Build (MSYS2 mingw64), from tests/standalone/:
 *   g++ -std=c++26 -O2 -I<engine>/include -I../.. -I../../.. Raster3DBench.cpp ../../Raster3D.cpp -o bench
 */

#include "../../Raster3D.h"
#include "../../Math3D.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace Deki3D;
using Deki::Mat4;
using Deki::Vector3;

namespace
{

// --- a subdivided cube, so triangle count is a dial ------------------------

struct MeshData
{
    std::vector<VertexPNTC> vertices;
    std::vector<uint16_t> indices;
    std::vector<Submesh3D> submeshes;

    Mesh3D View() const
    {
        Mesh3D m;
        m.vertices = vertices.data();
        m.vertexCount = static_cast<uint32_t>(vertices.size());
        m.layout = LayoutPNTC();
        m.indices = indices.data();
        m.indexCount = static_cast<uint32_t>(indices.size());
        m.submeshes = submeshes.data();
        m.submeshCount = static_cast<uint16_t>(submeshes.size());
        m.boundsMin = Vector3(-1, -1, -1);
        m.boundsMax = Vector3(1, 1, 1);
        return m;
    }
};

MeshData MakeCube(int divisions)
{
    MeshData mesh;
    // Six faces, each a grid of `divisions` quads a side. Winding is
    // counter-clockwise seen from outside, which is what the rasteriser
    // treats as front-facing.
    const Vector3 faceNormals[6] = {
        Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(1, 0, 0),
        Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0)
    };
    const Vector3 faceU[6] = {
        Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 0, -1),
        Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(1, 0, 0)
    };
    const Vector3 faceV[6] = {
        Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0),
        Vector3(0, 1, 0), Vector3(0, 0, -1), Vector3(0, 0, 1)
    };
    const uint32_t faceColors[6] = {
        0xFF4040FFu, 0xFF40FF40u, 0xFFFF4040u, 0xFF40FFFFu, 0xFFFF40FFu, 0xFFFFFF40u
    };

    for (int f = 0; f < 6; ++f)
    {
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        for (int j = 0; j <= divisions; ++j)
        {
            for (int i = 0; i <= divisions; ++i)
            {
                const float s = static_cast<float>(i) / divisions * 2.0f - 1.0f;
                const float t = static_cast<float>(j) / divisions * 2.0f - 1.0f;
                VertexPNTC v;
                v.position = faceNormals[f] + faceU[f] * s + faceV[f] * t;
                v.normal = faceNormals[f];
                v.u = static_cast<float>(i) / divisions;
                v.v = static_cast<float>(j) / divisions;
                v.color = faceColors[f];
                mesh.vertices.push_back(v);
            }
        }
        for (int j = 0; j < divisions; ++j)
        {
            for (int i = 0; i < divisions; ++i)
            {
                const uint16_t a = static_cast<uint16_t>(base + j * (divisions + 1) + i);
                const uint16_t b = static_cast<uint16_t>(a + 1);
                const uint16_t c = static_cast<uint16_t>(a + divisions + 1);
                const uint16_t d = static_cast<uint16_t>(c + 1);
                mesh.indices.insert(mesh.indices.end(), { a, b, d });
                mesh.indices.insert(mesh.indices.end(), { a, d, c });
            }
        }
    }

    Submesh3D sub;
    sub.firstIndex = 0;
    sub.indexCount = static_cast<uint32_t>(mesh.indices.size());
    sub.materialIndex = 0;
    mesh.submeshes.push_back(sub);
    return mesh;
}

// --- a checkerboard, so warping is visible ---------------------------------

struct CheckerTexture
{
    std::vector<uint16_t> pixels;
    Texture3D view;

    explicit CheckerTexture(int shift)
    {
        const int size = 1 << shift;
        pixels.resize(static_cast<size_t>(size) * size);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const bool on = ((x >> 3) ^ (y >> 3)) & 1;
                pixels[static_cast<size_t>(y) * size + x] = on ? 0xFFFF : 0x2104;
            }
        view.pixels = reinterpret_cast<const uint8_t*>(pixels.data());
        view.palette = nullptr;
        view.width = static_cast<uint16_t>(size);
        view.height = static_cast<uint16_t>(size);
        view.widthShift = static_cast<uint8_t>(shift);
        view.heightShift = static_cast<uint8_t>(shift);
    }
};

void WritePpm(const char* path, const std::vector<uint16_t>& fb, int w, int h)
{
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    for (int i = 0; i < w * h; ++i)
    {
        const uint16_t c = fb[i];
        const uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
        const uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
        const uint8_t b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
        out.put(static_cast<char>(r)).put(static_cast<char>(g)).put(static_cast<char>(b));
    }
}

uint16_t To565Ref(uint32_t rgba);

int g_Failures = 0;

void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("  FAIL  %s\n", what);
        ++g_Failures;
    }
    else
    {
        std::printf("  ok    %s\n", what);
    }
}

// --- correctness -----------------------------------------------------------

void CorrectnessChecks()
{
    std::printf("Correctness\n");

    const int W = 64, H = 64;
    std::vector<uint16_t> fb(static_cast<size_t>(W) * H, 0);

    RasterConfig cfg;
    cfg.tileSize = 16;

    // Two quads facing the camera, the red one nearer. Depth must decide.
    MeshData near = MakeCube(1);
    MeshData far = MakeCube(1);
    for (auto& v : near.vertices) v.color = 0xFF0000FFu;   // red
    for (auto& v : far.vertices) v.color = 0xFF00FF00u;    // green

    Material3D mat;
    mat.shading = ShadingModel::Unlit;

    const Mat4 proj = Perspective(1.0f, static_cast<float>(W) / H, 0.1f, 100.0f);
    const Mat4 view = LookAt(Vector3(0, 0, 6), Vector3(0, 0, 0), Vector3(0, 1, 0));
    const Mat4 vp = Mul(proj, view);

    Raster3D r;
    r.BeginFrame(reinterpret_cast<uint8_t*>(fb.data()), W, H, Deki::ColorFormat::RGB565, cfg);
    // far first, so a working depth test is what puts red on top
    r.DrawMesh(far.View(), &mat, 1, Mul(vp, Translate(0, 0, -1.5f)), Mat4::Identity());
    r.DrawMesh(near.View(), &mat, 1, Mul(vp, Translate(0, 0, 1.5f)), Mat4::Identity());
    r.EndFrame();

    const uint16_t center = fb[static_cast<size_t>(H / 2) * W + W / 2];
    Check(center == To565Ref(0xFF0000FFu), "the nearer surface wins the depth test");
    Check(fb[0] == 0, "the background is left alone");
    Check(r.Stats().trianglesIn > 0 && r.Stats().pixelsWritten > 0, "geometry reached the framebuffer");

    // Backface culling: a cube seen from outside shows three faces at most,
    // so the number of triangles surviving the clip is well under the total.
    Check(r.Stats().trianglesClipped < r.Stats().trianglesIn,
          "backfaces are culled before binning");

    WritePpm("raster_depth.ppm", fb, W, H);

    // Threading the fill must change the picture in no way at all. Tiles are
    // independent, so this is the property that makes it safe rather than
    // merely fast, and it is worth asserting rather than assuming.
    {
        const int TW = 160, TH = 120;
        MeshData mesh = MakeCube(6);
        Material3D m;
        m.shading = ShadingModel::VertexLit;
        const Mat4 tvp = Mul(Perspective(1.05f, static_cast<float>(TW) / TH, 0.1f, 100.0f),
                             LookAt(Vector3(0, 0, 3.2f), Vector3(0, 0, 0), Vector3(0, 1, 0)));
        const Mat4 model = Compose(0, 0, 0, 0.4f, 0.7f, 0, 1, 1, 1);

        std::vector<uint16_t> single(static_cast<size_t>(TW) * TH, 0);
        std::vector<uint16_t> many(static_cast<size_t>(TW) * TH, 0);
        uint32_t singleWritten = 0, manyWritten = 0;

        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<uint16_t>& target = pass == 0 ? single : many;
            RasterConfig c;
            c.tileSize = 32;
            c.threadCount = pass == 0 ? 1 : 4;
            Raster3D rr;
            rr.BeginFrame(reinterpret_cast<uint8_t*>(target.data()), TW, TH,
                          Deki::ColorFormat::RGB565, c);
            rr.DrawMesh(mesh.View(), &m, 1, Mul(tvp, model), Mat4::Identity());
            rr.EndFrame();
            (pass == 0 ? singleWritten : manyWritten) = rr.Stats().pixelsWritten;
        }

        Check(single == many, "four threads draw the same pixels as one");
        Check(singleWritten == manyWritten, "and report the same pixel count");
        Check(singleWritten > 1000, "the comparison actually drew something");

        // Changing the thread count after frames have already been drawn is
        // ordinary: a scene's value arrives with its component, one frame
        // after the first. It used to deadlock, because a worker created at
        // that point saw a frame serial it had never seen, decided a frame was
        // waiting for it and ran before it had been counted in. If this
        // regresses the test hangs rather than fails, which is the honest
        // signal for a deadlock.
        {
            std::vector<uint16_t> target(static_cast<size_t>(TW) * TH, 0);
            Raster3D rr;
            const int schedule[] = { 1, 1, 2, 4, 2, 1, 3 };
            for (int threads : schedule)
            {
                RasterConfig c;
                c.tileSize = 32;
                c.threadCount = threads;
                for (int frame = 0; frame < 3; ++frame)
                {
                    rr.BeginFrame(reinterpret_cast<uint8_t*>(target.data()), TW, TH,
                                  Deki::ColorFormat::RGB565, c);
                    rr.DrawMesh(mesh.View(), &m, 1, Mul(tvp, model), Mat4::Identity());
                    rr.EndFrame();
                }
            }
            Check(target == many, "the thread count can change between frames");
        }
    }

    // Bounds culling. The property that matters is that nothing visible is
    // ever culled; skipping a mesh that is genuinely off-screen is the easy
    // half.
    {
        const int CW = 128, CH = 128;
        std::vector<uint16_t> onscreen(static_cast<size_t>(CW) * CH, 0);
        std::vector<uint16_t> withOffscreen(static_cast<size_t>(CW) * CH, 0);
        MeshData cube = MakeCube(3);
        Material3D m;
        m.shading = ShadingModel::Unlit;

        const Mat4 cvp = Mul(Perspective(1.0f, 1.0f, 0.1f, 100.0f),
                             LookAt(Vector3(0, 0, 4), Vector3(0, 0, 0), Vector3(0, 1, 0)));
        RasterConfig c;
        c.tileSize = 32;

        Raster3D a;
        a.BeginFrame(reinterpret_cast<uint8_t*>(onscreen.data()), CW, CH,
                     Deki::ColorFormat::RGB565, c);
        a.DrawMesh(cube.View(), &m, 1, Mul(cvp, Translate(0, 0, 0)), Mat4::Identity());
        a.EndFrame();
        Check(a.Stats().meshesCulled == 0, "a mesh in view is not culled");
        Check(a.Stats().pixelsWritten > 0, "and it drew");

        // The same frame, plus meshes far outside every plane. The picture
        // must be identical and they must all have been skipped.
        Raster3D b;
        b.BeginFrame(reinterpret_cast<uint8_t*>(withOffscreen.data()), CW, CH,
                     Deki::ColorFormat::RGB565, c);
        b.DrawMesh(cube.View(), &m, 1, Mul(cvp, Translate(0, 0, 0)), Mat4::Identity());
        // Past each of the six planes. The far one has to clear 100 units
        // measured from the camera at z = 4, not from the origin; -90 is
        // still comfortably inside it.
        const Vector3 aside[] = { Vector3(-40, 0, 0), Vector3(40, 0, 0),   Vector3(0, -40, 0),
                                  Vector3(0, 40, 0),  Vector3(0, 0, -500), Vector3(0, 0, 40) };
        for (const Vector3& offset : aside)
            b.DrawMesh(cube.View(), &m, 1, Mul(cvp, Translate(offset.x, offset.y, offset.z)),
                       Mat4::Identity());
        b.EndFrame();

        Check(b.Stats().meshesCulled == 6, "six off-screen meshes were all culled");
        Check(b.Stats().trianglesIn == a.Stats().trianglesIn,
              "and cost no triangle work at all");
        Check(onscreen == withOffscreen, "the picture is unchanged by what was culled");

        // A mesh straddling the edge must survive: half of it is visible.
        Raster3D d;
        d.BeginFrame(reinterpret_cast<uint8_t*>(withOffscreen.data()), CW, CH,
                     Deki::ColorFormat::RGB565, c);
        d.DrawMesh(cube.View(), &m, 1, Mul(cvp, Translate(1.4f, 0, 0)), Mat4::Identity());
        d.EndFrame();
        Check(d.Stats().meshesCulled == 0, "a mesh straddling the edge is kept");
        Check(d.Stats().pixelsWritten > 0, "and still draws");
    }
}

uint16_t To565Ref(uint32_t rgba)
{
    const uint32_t r = (rgba >> 0) & 0xFF;
    const uint32_t g = (rgba >> 8) & 0xFF;
    const uint32_t b = (rgba >> 16) & 0xFF;
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// --- timing ----------------------------------------------------------------

struct Result
{
    double msClear;     // the framebuffer memset, which is not rasteriser work
    double msGeometry;  // transform, clip, project, bin
    double msFill;      // the span loops
    double nsPerPixel;  // fill only
    double nsPerTri;    // geometry only
    uint32_t pixelsWritten;
    uint32_t trianglesDrawn;

    double MsPerFrame() const { return msClear + msGeometry + msFill; }
};

Result TimeFrames(int width, int height, const MeshData& mesh, const Material3D& mat,
                  const RasterConfig& cfg, int frames, const char* ppmPath)
{
    std::vector<uint16_t> fb(static_cast<size_t>(width) * height, 0);
    Raster3D r;

    const Mat4 proj = Perspective(1.05f, static_cast<float>(width) / height, 0.1f, 100.0f);
    const Mat4 view = LookAt(Vector3(0, 0, 3.2f), Vector3(0, 0, 0), Vector3(0, 1, 0));
    const Mat4 vp = Mul(proj, view);

    // One untimed frame so first-touch page faults do not land in the timing.
    r.BeginFrame(reinterpret_cast<uint8_t*>(fb.data()), width, height, Deki::ColorFormat::RGB565, cfg);
    r.DrawMesh(mesh.View(), &mat, 1, Mul(vp, Compose(0, 0, 0, 0.4f, 0.7f, 0, 1, 1, 1)), Mat4::Identity());
    r.EndFrame();

    using Clock = std::chrono::steady_clock;
    auto Ns = [](Clock::duration d) {
        return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    };

    double clearNs = 0, geomNs = 0, fillNs = 0;
    uint64_t pixels = 0;
    uint64_t tris = 0;
    uint32_t lastTris = 0;

    for (int f = 0; f < frames; ++f)
    {
        const float angle = 0.7f + static_cast<float>(f) * 0.01f;
        const Mat4 model = Compose(0, 0, 0, 0.4f, angle, 0, 1, 1, 1);

        const auto t0 = Clock::now();
        std::memset(fb.data(), 0, fb.size() * sizeof(uint16_t));

        const auto t1 = Clock::now();
        r.BeginFrame(reinterpret_cast<uint8_t*>(fb.data()), width, height, Deki::ColorFormat::RGB565, cfg);
        r.DrawMesh(mesh.View(), &mat, 1, Mul(vp, model), Mat4::Identity());

        const auto t2 = Clock::now();
        r.EndFrame();
        const auto t3 = Clock::now();

        clearNs += Ns(t1 - t0);
        geomNs += Ns(t2 - t1);
        fillNs += Ns(t3 - t2);
        pixels += r.Stats().pixelsWritten;
        tris += r.Stats().trianglesIn;
        lastTris = r.Stats().trianglesClipped;
    }

    if (ppmPath)
        WritePpm(ppmPath, fb, width, height);

    Result res;
    res.msClear = clearNs / frames / 1e6;
    res.msGeometry = geomNs / frames / 1e6;
    res.msFill = fillNs / frames / 1e6;
    res.nsPerPixel = pixels ? fillNs / static_cast<double>(pixels) : 0.0;
    res.nsPerTri = tris ? geomNs / static_cast<double>(tris) : 0.0;
    res.pixelsWritten = static_cast<uint32_t>(pixels / frames);
    res.trianglesDrawn = lastTris;
    return res;
}

}  // namespace

int main()
{
    CorrectnessChecks();

    std::printf("\nTimings (this desktop, one core, -O2)\n");
    std::printf("%-33s %8s %7s %7s %7s %8s %8s\n",
                "case", "ms/frame", "clear", "geom", "fill", "ns/pixel", "ns/tri");

    CheckerTexture checker(6);  // 64x64
    Material3D textured;
    textured.texture = &checker.view;
    textured.shading = ShadingModel::Unlit;

    Material3D texturedLit = textured;
    texturedLit.shading = ShadingModel::VertexLit;

    const MeshData lowPoly = MakeCube(2);    // 96 triangles
    const MeshData midPoly = MakeCube(12);   // ~3.5k triangles
    const MeshData highPoly = MakeCube(40);  // ~38k triangles

    struct Case
    {
        const char* name;
        int w, h;
        const MeshData* mesh;
        const Material3D* mat;
        RasterConfig cfg;
        int frames;
        const char* ppm;
    };

    RasterConfig device;
    device.tileSize = 32;

    RasterConfig affine = device;
    affine.perspectiveCorrect = false;

    RasterConfig retro = device;
    retro.perspectiveCorrect = false;
    retro.vertexSnap = true;

    RasterConfig desktopTiles = device;
    desktopTiles.tileSize = 128;

    // Threading the fill: the axis a dual-core microcontroller would use, and
    // the one the desktop numbers below can actually measure.
    RasterConfig twoThreads = desktopTiles;
    twoThreads.threadCount = 2;
    RasterConfig fourThreads = desktopTiles;
    fourThreads.threadCount = 4;
    RasterConfig deviceTwoThreads = device;
    deviceTwoThreads.threadCount = 2;

    const Case cases[] = {
        { "320x240 low poly, textured",   320, 240, &lowPoly,  &textured,     device,       300, "raster_320.ppm" },
        { "320x240 low poly, affine",     320, 240, &lowPoly,  &textured,     affine,       300, "raster_affine.ppm" },
        { "320x240 low poly, retro snap", 320, 240, &lowPoly,  &textured,     retro,        300, "raster_retro.ppm" },
        { "320x240 low poly, vertex lit", 320, 240, &lowPoly,  &texturedLit,  device,       300, nullptr },
        { "320x240 mid poly, textured",   320, 240, &midPoly,  &textured,     device,       200, nullptr },
        { "320x240 high poly, textured",  320, 240, &highPoly, &textured,     device,       100, nullptr },
        { "1920x1080 low poly, textured", 1920, 1080, &lowPoly, &textured,    desktopTiles, 100, "raster_1080.ppm" },
        { "1920x1080 mid poly, textured", 1920, 1080, &midPoly, &textured,    desktopTiles, 60,  nullptr },
        { "1920x1080 high poly, textured",1920, 1080, &highPoly, &textured,   desktopTiles, 40,  nullptr },
        { "1920x1080 low poly, 2 threads", 1920, 1080, &lowPoly, &textured,    twoThreads,   100, nullptr },
        { "1920x1080 low poly, 4 threads", 1920, 1080, &lowPoly, &textured,    fourThreads,  100, nullptr },
        { "320x240 low poly, 2 threads",   320, 240, &lowPoly,  &textured,     deviceTwoThreads, 300, nullptr },
    };

    for (const Case& c : cases)
    {
        const Result r = TimeFrames(c.w, c.h, *c.mesh, *c.mat, c.cfg, c.frames, c.ppm);
        std::printf("%-33s %8.3f %7.3f %7.3f %7.3f %8.2f %8.1f\n", c.name,
                    r.MsPerFrame(), r.msClear, r.msGeometry, r.msFill,
                    r.nsPerPixel, r.nsPerTri);
    }

    std::printf("\n%d check(s) failed\n", g_Failures);
    return g_Failures ? 1 : 0;
}
