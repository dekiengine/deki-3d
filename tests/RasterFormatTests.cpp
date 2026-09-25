/**
 * @file RasterFormatTests.cpp
 * @brief deki-3d reads every texture format and writes every framebuffer
 *        format: the same textured quad comes out the same colours in each
 *        (within what the narrower formats can hold), alpha cuts out where the
 *        material asks, and version 3 mesh files still load.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../MeshAsset.h"
#include "../Raster3D.h"
#include "../editor/ObjImporter.h"

using namespace Deki3D;

namespace
{

constexpr int kSize = 32;     // framebuffer is kSize x kSize
constexpr int kTexShift = 3;  // 8 x 8 texture
constexpr int kTex = 1 << kTexShift;

// An 8x8 texture of four colour quadrants; the right half is transparent when
// `holes` is set.
std::vector<uint8_t> SourceRgba(bool holes)
{
    std::vector<uint8_t> rgba(kTex * kTex * 4);
    for (int y = 0; y < kTex; ++y)
        for (int x = 0; x < kTex; ++x)
        {
            uint8_t* p = &rgba[(y * kTex + x) * 4];
            const bool right = x >= kTex / 2, bottom = y >= kTex / 2;
            p[0] = right ? 200 : 40;
            p[1] = bottom ? 180 : 60;
            p[2] = (right != bottom) ? 220 : 20;
            p[3] = (holes && right) ? 0 : 255;
        }
    return rgba;
}

// The texels of `rgba` in `format`, as the importer stores them.
std::vector<uint8_t> Encode(const std::vector<uint8_t>& rgba, TexelFormat format)
{
    std::vector<uint8_t> out(kTex * kTex * TexelBytes(format));
    for (int i = 0; i < kTex * kTex; ++i)
    {
        const uint8_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        uint8_t* d = &out[i * TexelBytes(format)];
        switch (format)
        {
            case TexelFormat::RGB565: d[0] = v & 0xFF; d[1] = v >> 8; break;
            case TexelFormat::RGB565A8: d[0] = v & 0xFF; d[1] = v >> 8; d[2] = a; break;
            case TexelFormat::RGB888: d[0] = r; d[1] = g; d[2] = b; break;
            case TexelFormat::RGBA8888: d[0] = r; d[1] = g; d[2] = b; d[3] = a; break;
            case TexelFormat::ALPHA8: d[0] = a; break;
        }
    }
    return out;
}

// One pixel of the framebuffer as 8-bit RGB.
void ReadPixel(const std::vector<uint8_t>& fb, Deki::ColorFormat format, int x, int y, int rgb[3])
{
    const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
    const uint8_t* p = &fb[(static_cast<size_t>(y) * kSize + x) * bpp];
    if (format == Deki::ColorFormat::RGB888)
    {
        rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
    }
    else if (format == Deki::ColorFormat::ARGB8888)
    {
        rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0];
    }
    else
    {
        const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
        rgb[0] = ((v >> 11) & 0x1F) << 3;
        rgb[1] = ((v >> 5) & 0x3F) << 2;
        rgb[2] = (v & 0x1F) << 3;
    }
}

// A unit quad facing the camera, drawn with an orthographic projection that
// maps it onto the whole framebuffer. Unlit, so what is sampled is written.
std::vector<uint8_t> Render(const Texture3D& tex, Deki::ColorFormat format, bool alphaTest)
{
    const uint32_t white = 0xFFFFFFFFu;
    const VertexPNTC vertices[4] = {
        { Deki::Vector3(-1, -1, 0), Deki::Vector3(0, 0, 1), 0.0f, 1.0f, white },
        { Deki::Vector3(1, -1, 0), Deki::Vector3(0, 0, 1), 1.0f, 1.0f, white },
        { Deki::Vector3(1, 1, 0), Deki::Vector3(0, 0, 1), 1.0f, 0.0f, white },
        { Deki::Vector3(-1, 1, 0), Deki::Vector3(0, 0, 1), 0.0f, 0.0f, white },
    };
    const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    Submesh3D sub;
    sub.indexCount = 6;

    Mesh3D mesh;
    mesh.vertices = vertices;
    mesh.vertexCount = 4;
    mesh.layout = LayoutPNTC();
    mesh.indices = indices;
    mesh.indexCount = 6;
    mesh.submeshes = &sub;
    mesh.submeshCount = 1;
    mesh.boundsMin = Deki::Vector3(-1, -1, 0);
    mesh.boundsMax = Deki::Vector3(1, 1, 0);

    Material3D material;
    material.texture = &tex;
    material.doubleSided = true;
    material.alphaTest = alphaTest;

    // Cleared to a colour no texel has, so a hole is visible.
    std::vector<uint8_t> fb(Deki::FrameBufferBytes(format, kSize, kSize), 0);
    RasterConfig config;
    config.tileSize = 16;
    Raster3D raster;
    raster.BeginFrame(fb.data(), kSize, kSize, format, config);
    const Deki::Mat4 mvp = Deki::Mat4::Ortho(-1, 1, -1, 1, -1, 1);
    raster.DrawMesh(mesh, &material, 1, mvp, Deki::Mat4::Identity());
    raster.EndFrame();
    return fb;
}

Texture3D View(const std::vector<uint8_t>& pixels, TexelFormat format)
{
    Texture3D t;
    t.pixels = pixels.data();
    t.format = format;
    t.width = t.height = kTex;
    t.widthShift = t.heightShift = kTexShift;
    return t;
}

const TexelFormat kColourTexels[] = { TexelFormat::RGB565, TexelFormat::RGB565A8, TexelFormat::RGB888,
                                      TexelFormat::RGBA8888 };
const Deki::ColorFormat kTargets[] = { Deki::ColorFormat::RGB565, Deki::ColorFormat::RGB888,
                                       Deki::ColorFormat::ARGB8888, Deki::ColorFormat::RGB565A8 };

}  // namespace

// Every colour texture into every framebuffer: the same picture as RGB565
// into RGB565, give or take the 565 quantisation (under 8 per channel).
TEST(RasterFormats, EveryTextureIntoEveryFramebufferMatches)
{
    const std::vector<uint8_t> rgba = SourceRgba(false);
    const std::vector<uint8_t> refTexels = Encode(rgba, TexelFormat::RGB565);
    const std::vector<uint8_t> reference = Render(View(refTexels, TexelFormat::RGB565), Deki::ColorFormat::RGB565, false);

    int covered = 0;
    for (TexelFormat tf : kColourTexels)
    {
        const std::vector<uint8_t> texels = Encode(rgba, tf);
        for (Deki::ColorFormat target : kTargets)
        {
            const std::vector<uint8_t> fb = Render(View(texels, tf), target, false);
            for (int y = 0; y < kSize; ++y)
                for (int x = 0; x < kSize; ++x)
                {
                    int want[3], got[3];
                    ReadPixel(reference, Deki::ColorFormat::RGB565, x, y, want);
                    ReadPixel(fb, target, x, y, got);
                    for (int c = 0; c < 3; ++c)
                        ASSERT_LE(std::abs(want[c] - got[c]), 8)
                            << "texel " << static_cast<int>(tf) << " target " << static_cast<int>(target) << " at "
                            << x << "," << y;
                    if (want[0] || want[1] || want[2])
                        ++covered;
                }
        }
    }
    EXPECT_GT(covered, 0) << "the quad drew nothing";
}

// RGB565 into RGB565 is the path the rasteriser has always had: the colours
// are the texture's 565 values exactly.
TEST(RasterFormats, Rgb565IntoRgb565IsExact)
{
    const std::vector<uint8_t> texels = Encode(SourceRgba(false), TexelFormat::RGB565);
    const std::vector<uint8_t> fb = Render(View(texels, TexelFormat::RGB565), Deki::ColorFormat::RGB565, false);
    const uint16_t* px = reinterpret_cast<const uint16_t*>(fb.data());
    const uint16_t* tx = reinterpret_cast<const uint16_t*>(texels.data());
    // The top-left and bottom-right screen corners sample the texture's corners.
    EXPECT_EQ(px[1 * kSize + 1], tx[0]);
    EXPECT_EQ(px[(kSize - 2) * kSize + (kSize - 2)], tx[kTex * kTex - 1]);
}

// With alphaTest, transparent texels leave the framebuffer as it was; without
// it they draw their colour.
TEST(RasterFormats, AlphaCutsOutWhereTheMaterialAsks)
{
    const std::vector<uint8_t> rgba = SourceRgba(true);
    for (TexelFormat tf : { TexelFormat::RGB565A8, TexelFormat::RGBA8888 })
    {
        const std::vector<uint8_t> texels = Encode(rgba, tf);
        for (Deki::ColorFormat target : kTargets)
        {
            const std::vector<uint8_t> cut = Render(View(texels, tf), target, true);
            const std::vector<uint8_t> solid = Render(View(texels, tf), target, false);
            int left[3], right[3], rightSolid[3];
            ReadPixel(cut, target, kSize / 4, kSize / 4, left);
            ReadPixel(cut, target, kSize * 3 / 4, kSize / 4, right);
            ReadPixel(solid, target, kSize * 3 / 4, kSize / 4, rightSolid);
            EXPECT_TRUE(left[0] || left[1] || left[2]) << "the opaque half drew";
            EXPECT_TRUE(right[0] == 0 && right[1] == 0 && right[2] == 0) << "the transparent half is a hole";
            EXPECT_TRUE(rightSolid[0] || rightSolid[1] || rightSolid[2]) << "no alpha test, no hole";
        }
    }
}

// Alpha-only textures take their colour from the material and their coverage
// from the texel.
TEST(RasterFormats, Alpha8DrawsTheMaterialColourWhereCovered)
{
    const std::vector<uint8_t> texels = Encode(SourceRgba(true), TexelFormat::ALPHA8);
    const std::vector<uint8_t> fb = Render(View(texels, TexelFormat::ALPHA8), Deki::ColorFormat::ARGB8888, true);
    int left[3], right[3];
    ReadPixel(fb, Deki::ColorFormat::ARGB8888, kSize / 4, kSize / 4, left);
    ReadPixel(fb, Deki::ColorFormat::ARGB8888, kSize * 3 / 4, kSize / 4, right);
    EXPECT_EQ(left[0], 255);  // the white vertex colour and tint
    EXPECT_EQ(right[0], 0);
}

// A version 3 file (no format in the texture table) still loads, as RGB565.
TEST(MeshFile, Version3StillLoads)
{
    std::vector<uint8_t> blob;
    auto put = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        blob.insert(blob.end(), b, b + n);
    };

    MeshFileHeader h{};
    std::memcpy(h.magic, "DMSH", 4);
    h.version = 3;
    h.attributes = MeshAttribute_Position;
    h.vertexCount = 3;
    h.indexCount = 3;
    h.vertexStride = 12;
    h.materialCount = 1;
    h.textureCount = 1;
    h.texturePixelBytes = 2 * 2 * 2;
    put(&h, sizeof(h));
    const float positions[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    put(positions, sizeof(positions));
    const uint16_t indices[3] = { 0, 1, 2 };
    put(indices, sizeof(indices));
    const MeshFileMaterial material{ 0, 0, 0xFFFFFFFFu };
    put(&material, sizeof(material));
    const MeshFileTextureV3 texture{ 2, 2, 0 };
    put(&texture, sizeof(texture));
    const uint16_t pixels[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    put(pixels, sizeof(pixels));

    MeshAsset mesh;
    ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
    ASSERT_EQ(mesh.TextureCount(), 1);
    const Texture3D* t = mesh.Materials()[0].texture;
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->Valid());
    EXPECT_EQ(t->format, TexelFormat::RGB565);
    EXPECT_EQ(reinterpret_cast<const uint16_t*>(t->pixels)[0], 0xF800);
}

// The importer stores a texture in the format it is asked for, and a texture
// with transparent texels turns the material's alpha test on.
TEST(MeshFile, ImporterStoresTheChosenFormat)
{
    const char* obj =
        "mtllib m.mtl\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "usemtl a\n"
        "f 1/1 2/2 3/3\n";
    const std::string mtl = "newmtl a\nmap_Kd t.png\n";
    // A decoder that serves the .mtl's texture from memory: 8x8 with holes.
    auto decode = [](const std::string&, int& w, int& h, std::vector<uint8_t>& rgba) {
        w = h = kTex;
        rgba = SourceRgba(true);
        return true;
    };
    // The .mtl is read from disk relative to the base directory, so write it.
    const std::string dir = testing::TempDir();
    {
        FILE* f = std::fopen((dir + "/m.mtl").c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(mtl.data(), 1, mtl.size(), f);
        std::fclose(f);
    }

    for (TexelFormat format : { TexelFormat::RGB565, TexelFormat::RGBA8888 })
    {
        std::vector<uint8_t> blob;
        std::string error;
        ASSERT_TRUE(CompileObjToMesh(obj, dir, decode, blob, error, [&](bool hasAlpha) {
            EXPECT_TRUE(hasAlpha);
            return format;
        })) << error;
        MeshAsset mesh;
        ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
        const Material3D& m = mesh.Materials()[0];
        ASSERT_NE(m.texture, nullptr);
        EXPECT_EQ(m.texture->format, format);
        EXPECT_EQ(m.alphaTest, format == TexelFormat::RGBA8888) << "only a format that keeps alpha cuts out";
    }
}
