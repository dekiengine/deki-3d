// Tests that a mesh of any size compiles, loads and draws: 16-bit indices
// while they reach every vertex, 32-bit past 65535 vertices, and the version
// 4 files from before still load. And the parts of a model a GPU target keeps
// that a board drops: filtered textures, a map_d cutout.

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

#include <deki/providers/FileSystem.h>
#include <deki/providers/IFileSystem.h>
#include <deki/providers/HostMemoryProvider.h>
#include <deki/providers/Memory.h>
#include <deki/assets/AssetManager.h>

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
    options.textureFor = [](const std::string&)
    {
        MeshTextureChoice choice;
        choice.format = [](bool hasAlpha) { return hasAlpha ? TexelFormat::RGBA8888 : TexelFormat::RGB888; };
        return choice;
    };
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

// Each texture carries its image's filter, asked for by the image's path.
TEST(BigMesh, TexturesCarryTheirImagesFilter)
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
    for (Deki::TextureFilter filter :
         { Deki::TextureFilter::Point, Deki::TextureFilter::Bilinear, Deki::TextureFilter::Trilinear })
    {
        MeshCompileOptions options;
        std::vector<std::string> asked;
        options.textureFor = [&](const std::string& imagePath)
        {
            asked.push_back(imagePath);
            MeshTextureChoice choice;
            choice.filter = filter;
            return choice;
        };
        std::vector<uint8_t> blob;
        std::string error;
        ASSERT_TRUE(CompileObjToMesh(obj, dir, decode, blob, error, options)) << error;
        MeshAsset mesh;
        ASSERT_TRUE(mesh.LoadFromMemory(blob.data(), blob.size()));
        ASSERT_NE(mesh.Materials()[0].texture, nullptr);
        EXPECT_EQ(mesh.Materials()[0].texture->filter, filter);
        ASSERT_EQ(asked.size(), 1u);
        EXPECT_NE(asked[0].find("t.png"), std::string::npos) << asked[0];
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
        options.textureFor = [cap](const std::string&)
        {
            MeshTextureChoice choice;
            choice.maxSize = cap;
            return choice;
        };
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

namespace
{

// The engine's file system over stdio, enough for a test to load a file.
class StdioFileSystem : public Deki::IFileSystem
{
public:
    bool Initialize() override { return true; }
    void Shutdown() override {}
    FileHandle OpenFile(const char* path, OpenMode mode) override
    {
        return mode == OpenMode::ReadBinary ? std::fopen(path, "rb") : nullptr;
    }
    void CloseFile(FileHandle handle) override { std::fclose(static_cast<FILE*>(handle)); }
    size_t ReadFile(FileHandle handle, void* buffer, size_t size) override
    {
        ++reads;
        return std::fread(buffer, 1, size, static_cast<FILE*>(handle));
    }
    size_t WriteFile(FileHandle, const void*, size_t) override { return 0; }
    long SeekFile(FileHandle handle, long offset, SeekOrigin origin) override
    {
        const int whence = origin == SeekOrigin::Begin ? SEEK_SET : origin == SeekOrigin::Current ? SEEK_CUR : SEEK_END;
        return std::fseek(static_cast<FILE*>(handle), offset, whence);
    }
    long TellFile(FileHandle handle) override { return std::ftell(static_cast<FILE*>(handle)); }
    long GetFileSize(FileHandle handle) override
    {
        FILE* f = static_cast<FILE*>(handle);
        const long at = std::ftell(f);
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, at, SEEK_SET);
        return size;
    }
    bool FileExists(const char* path) override
    {
        FILE* f = std::fopen(path, "rb");
        if (f)
        {
            std::fclose(f);
        }
        return f != nullptr;
    }
    bool ConvertPath(const char* virtualPath, char* out, size_t size) override
    {
        std::snprintf(out, size, "%s", virtualPath);
        return true;
    }
    int reads = 0;
};

}  // namespace

// A mesh read from a file takes its own size in memory while it loads, not
// the file's size again besides: the build's check of a board's memory counts
// just the mesh. Read part by part, and the same mesh as from memory.
TEST(BigMesh, LoadingFromAFileTakesOnlyTheMeshsMemory)
{
    std::vector<uint8_t> blob;
    std::string error;
    ASSERT_TRUE(CompileObjToMesh(GridObj(300), "", {}, blob, error)) << error;
    const std::string path = testing::TempDir() + "/streamed.mesh";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(blob.data(), 1, blob.size(), f);
        std::fclose(f);
    }
    auto* fs = new StdioFileSystem();
    ASSERT_TRUE(Deki::FileSystem::SetFileSystem(fs));

    // The most External memory held while loading, above what was held
    // before: from the file, and as the loader did before, the whole file
    // read in and then parsed.
    // Counted only while the engine's memory layer runs, as it does on a
    // board; a plain test binary has none.
    struct CountedMemory
    {
        CountedMemory()
        {
            Deki::Memory::SetBackend(new Deki::HostMemoryProvider());  // takes ownership
            Deki::Memory::Initialize();
        }
        ~CountedMemory()
        {
            Deki::Memory::Shutdown();
            Deki::Memory::SetBackend(nullptr);
        }
    } counted;
    auto peakOf = [](const auto& load)
    {
        Deki::Memory::ResetPeaks();
        const uint32_t base = Deki::Memory::GetPeakExternal();
        load();
        return Deki::Memory::GetPeakExternal() - base;
    };
    MeshAsset fromFile;
    const uint32_t streamed = peakOf([&] { ASSERT_TRUE(fromFile.LoadFromFile(path.c_str())); });
    const uint32_t whole = peakOf(
        [&]
        {
            Deki::Buffer<uint8_t> bytes;
            ASSERT_TRUE(Deki::AssetManager::ReadWholeFile(path, bytes, Deki::Memory::External));
            MeshAsset copy;
            ASSERT_TRUE(copy.LoadFromMemory(bytes.Data(), bytes.Count()));
        });
    EXPECT_GT(fs->reads, 3) << "read a part at a time";
    ASSERT_GT(whole, 0u) << "this build counts External memory";
    EXPECT_LT(streamed, blob.size() * 1.1) << "the file is not held besides the mesh";
    EXPECT_GT(whole, blob.size() * 1.9) << "the old way held it twice";

    MeshAsset fromMemory;
    ASSERT_TRUE(fromMemory.LoadFromMemory(blob.data(), blob.size()));
    EXPECT_EQ(fromFile.VertexCount(), fromMemory.VertexCount());
    EXPECT_EQ(fromFile.TriangleCount(), fromMemory.TriangleCount());
    EXPECT_EQ(fromFile.View().indexBytes, fromMemory.View().indexBytes);
    EXPECT_EQ(std::memcmp(fromFile.View().vertices, fromMemory.View().vertices,
                          static_cast<size_t>(fromFile.VertexCount()) * fromFile.View().layout.stride),
              0);

    // A file cut short fails cleanly.
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(blob.data(), 1, blob.size() / 2, f);
        std::fclose(f);
    }
    MeshAsset truncated;
    EXPECT_FALSE(truncated.LoadFromFile(path.c_str()));
    EXPECT_FALSE(truncated.Valid());
    EXPECT_FALSE(MeshAsset().LoadFromFile((path + ".missing").c_str()));
}

// What a model is compiled from besides itself: its material library and the
// images its materials name, so a change to one rebuilds it.
TEST(BigMesh, SourceFilesAreTheLibraryAndItsImages)
{
    const std::string dir = testing::TempDir();
    WriteFile(dir + "/src.mtl", "newmtl a\nmap_Kd tex\\a.png\nmap_d mask.png\nnewmtl b\nmap_Kd tex/a.png\n");
    const std::vector<std::string> files = ObjSourceFiles("mtllib src.mtl\nv 0 0 0\n", dir);
    ASSERT_EQ(files.size(), 3u);
    EXPECT_NE(files[0].find("src.mtl"), std::string::npos) << files[0];
    EXPECT_NE(files[1].find("a.png"), std::string::npos) << files[1];
    EXPECT_NE(files[2].find("mask.png"), std::string::npos) << files[2];
    EXPECT_TRUE(ObjSourceFiles("v 0 0 0\n", dir).empty()) << "no library, nothing besides";
}
