// Tests that a mesh of any size compiles, loads and draws: 16-bit indices
// while they reach every vertex, 32-bit past 65535 vertices, and the version
// 4 files from before still load. And the parts of a model a GPU target keeps
// that a board drops: smooth textures, a map_d cutout.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "../MeshAsset.h"
#include "../Raster3D.h"
#include "../editor/ObjImporter.h"

using namespace Deki3D;

namespace
{

// A flat grid of side x side vertices, as .obj text.
std::string GridObj(int side)
{
    std::ostringstream obj;
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            obj << "v " << x << " " << y << " 0\n";
        }
    }
    for (int y = 0; y + 1 < side; ++y)
    {
        for (int x = 0; x + 1 < side; ++x)
        {
            const int a = y * side + x + 1;  // .obj counts from 1
            obj << "f " << a << " " << a + 1 << " " << a + side + 1 << " " << a + side << "\n";
        }
    }
    return obj.str();
}

const MeshFileHeader& HeaderOf(const std::vector<uint8_t>& blob)
{
    return *reinterpret_cast<const MeshFileHeader*>(blob.data());
}

constexpr int kSize = 32;

// A quad over the whole framebuffer, its four vertices at `first` onwards in
// a buffer padded with unused ones before them, drawn with `Index` indices.
template <typename Index>
std::vector<uint8_t> RenderQuadAt(uint32_t first)
{
    std::vector<VertexPNTC> vertices(first + 4);
    const uint32_t colours[4] = { 0xFF2040E0u, 0xFF20E040u, 0xFFE04020u, 0xFFE0E020u };
    const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int k = 0; k < 4; ++k)
    {
        VertexPNTC& v = vertices[first + k];
        v.position = Deki::Vector3(corners[k][0], corners[k][1], 0);
        v.normal = Deki::Vector3(0, 0, 1);
        v.u = v.v = 0;
        v.color = colours[k];
    }
    std::vector<Index> indices;
    for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u })
    {
        indices.push_back(static_cast<Index>(first + i));
    }
    Submesh3D sub;
    sub.indexCount = 6;
    Mesh3D mesh;
    mesh.vertices = vertices.data();
    mesh.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.layout = LayoutPNTC();
    mesh.indices = indices.data();
    mesh.indexBytes = sizeof(Index);
    mesh.indexCount = 6;
    mesh.submeshes = &sub;
    mesh.submeshCount = 1;
    mesh.boundsMin = Deki::Vector3(-1, -1, 0);
    mesh.boundsMax = Deki::Vector3(1, 1, 0);

    Material3D material;
    material.doubleSided = true;
    std::vector<uint8_t> fb(Deki::FrameBufferBytes(Deki::ColorFormat::RGB565, kSize, kSize), 0);
    RasterConfig config;
    config.tileSize = 16;
    Raster3D raster;
    raster.BeginFrame(fb.data(), kSize, kSize, Deki::ColorFormat::RGB565, config);
    raster.DrawMesh(mesh, &material, 1, Deki::Mat4::Ortho(-1, 1, -1, 1, -1, 1), Deki::Mat4::Identity());
    raster.EndFrame();
    return fb;
}

void WriteFile(const std::string& path, const std::string& text)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr) << path;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

}  // namespace

TEST(BigMesh, ASmallModelKeeps16BitIndices)
{
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(GridObj(10), "", {}, blob, error)) << error;
    EXPECT_EQ(HeaderOf(blob).version, kMeshFileVersion);
    EXPECT_EQ(HeaderOf(blob).flags & MeshFileIndex32, 0u);
    MeshAsset mesh;
    ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
    EXPECT_EQ(mesh.View().indexBytes, 2);
}

TEST(BigMesh, AModelPast65535VerticesCompilesWith32BitIndices)
{
    const int side = 300;  // 90000 vertices
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(GridObj(side), "", {}, blob, error)) << error;
    EXPECT_NE(HeaderOf(blob).flags & MeshFileIndex32, 0u);

    MeshAsset mesh;
    ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
    const Mesh3D& view = mesh.View();
    EXPECT_EQ(view.indexBytes, 4);
    EXPECT_EQ(mesh.VertexCount(), static_cast<uint32_t>(side * side));
    EXPECT_EQ(mesh.TriangleCount(), static_cast<uint32_t>((side - 1) * (side - 1) * 2));
    uint32_t highest = 0;
    for (uint32_t i = 0; i < view.indexCount; ++i)
    {
        highest = std::max(highest, view.IndexAt(i));
    }
    EXPECT_EQ(highest, static_cast<uint32_t>(side * side - 1)) << "the last vertex is reached";
}

TEST(BigMesh, AnIndexPastTheVerticesIsRefusedAt32Bits)
{
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(GridObj(300), "", {}, blob, error)) << error;
    const MeshFileHeader& h = HeaderOf(blob);
    uint32_t* indices = reinterpret_cast<uint32_t*>(blob.data() + sizeof(MeshFileHeader) +
                                                    static_cast<size_t>(h.vertexCount) * h.vertexStride);
    indices[5] = h.vertexCount;
    MeshAsset mesh;
    EXPECT_FALSE(mesh.LoadFromMemory(blob.data(), blob.size()));
}

TEST(BigMesh, AVersion4FileStillLoads)
{
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(GridObj(10), "", {}, blob, error)) << error;
    // Version 4 is version 5 without the header's flags.
    std::vector<uint8_t> v4(blob.begin(), blob.begin() + kMeshFileHeaderV4Bytes);
    v4.insert(v4.end(), blob.begin() + sizeof(MeshFileHeader), blob.end());
    reinterpret_cast<MeshFileHeader*>(v4.data())->version = 4;

    MeshAsset mesh;
    ASSERT_TRUE(mesh.LoadFromMemory(v4.data(), v4.size()));
    EXPECT_EQ(mesh.VertexCount(), 100u);
    EXPECT_EQ(mesh.View().indexBytes, 2);
}

TEST(BigMesh, TheRasteriserDrawsVerticesPast65535)
{
    const std::vector<uint8_t> small = RenderQuadAt<uint16_t>(0);
    const std::vector<uint8_t> wide = RenderQuadAt<uint32_t>(0);
    const std::vector<uint8_t> far = RenderQuadAt<uint32_t>(70000);
    EXPECT_NE(small, std::vector<uint8_t>(small.size(), 0)) << "the quad drew";
    EXPECT_EQ(small, wide) << "32-bit indices draw what 16-bit ones do";
    EXPECT_EQ(small, far) << "vertices past 65535 are reached";
}

TEST(BigMesh, MapDCutsOutAndWindowsPathsResolve)
{
    const std::string dir = testing::TempDir();
    WriteFile(dir + "/m.mtl", "newmtl a\nmap_Kd textures\\colour.png\nmap_d textures\\mask.png\n");
    const std::string obj = "mtllib m.mtl\n"
                            "v 0 0 0\nv 1 0 0\nv 1 1 0\n"
                            "vt 0 0\nvt 1 0\nvt 1 1\n"
                            "usemtl a\n"
                            "f 1/1 2/2 3/3\n";
    std::vector<std::string> asked;
    auto decode = [&](const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba)
    {
        asked.push_back(path);
        w = h = 2;
        rgba.assign(16, 255);
        if (path.find("mask") != std::string::npos)
        {
            rgba[0] = rgba[1] = rgba[2] = 0;  // the first texel is a hole
        }
        return true;
    };
    MeshCompileOptions options;
    options.chooseFormat = [](bool hasAlpha) { return hasAlpha ? TexelFormat::RGBA8888 : TexelFormat::RGB888; };
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(obj, dir, decode, blob, error, options)) << error;

    ASSERT_EQ(asked.size(), 2u);
    for (const std::string& path : asked)
    {
        EXPECT_EQ(path.find('\\', dir.size()), std::string::npos) << path << " still has a backslash";
    }
    MeshAsset mesh;
    ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
    const Material3D& m = mesh.Materials()[0];
    ASSERT_NE(m.texture, nullptr);
    EXPECT_EQ(m.texture->format, TexelFormat::RGBA8888);
    EXPECT_TRUE(m.alphaTest) << "the mask made holes, so the material cuts out";
    EXPECT_EQ(m.texture->pixels[3], 0) << "the mask's black texel is a hole";
    EXPECT_EQ(m.texture->pixels[7], 255);
}

TEST(BigMesh, SmoothTexturesAreMarked)
{
    const std::string dir = testing::TempDir();
    WriteFile(dir + "/s.mtl", "newmtl a\nmap_Kd t.png\n");
    const std::string obj = "mtllib s.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nusemtl a\nf 1/1 2/2 3/3\n";
    auto decode = [](const std::string&, int& w, int& h, std::vector<uint8_t>& rgba)
    {
        w = h = 4;
        rgba.assign(64, 200);
        return true;
    };
    for (bool smooth : { false, true })
    {
        MeshCompileOptions options;
        options.smoothTextures = smooth;
        std::vector<uint8_t> blob;
        std::string error;
        ASSERT_TRUE(CompileObjToMesh(obj, dir, decode, blob, error, options)) << error;
        MeshAsset mesh;
        ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
        ASSERT_NE(mesh.Materials()[0].texture, nullptr);
        EXPECT_EQ(mesh.Materials()[0].texture->smooth, smooth);
    }
}

TEST(BigMesh, NoCapKeepsTheSourceSize)
{
    const std::string dir = testing::TempDir();
    WriteFile(dir + "/c.mtl", "newmtl a\nmap_Kd t.png\n");
    const std::string obj = "mtllib c.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nusemtl a\nf 1/1 2/2 3/3\n";
    auto decode = [](const std::string&, int& w, int& h, std::vector<uint8_t>& rgba)
    {
        w = 1024;
        h = 512;
        rgba.assign(static_cast<size_t>(w) * h * 4, 128);
        return true;
    };
    for (int cap : { 0, 256 })
    {
        MeshCompileOptions options;
        options.maxTextureSize = cap;
        std::vector<uint8_t> blob;
        std::string error;
        ASSERT_TRUE(CompileObjToMesh(obj, dir, decode, blob, error, options)) << error;
        MeshAsset mesh;
        ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
        const Texture3D* t = mesh.Materials()[0].texture;
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(t->width, cap == 0 ? 1024 : 256);
        EXPECT_EQ(t->height, cap == 0 ? 512 : 256);
    }
}
