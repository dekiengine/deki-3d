// A glTF model, read into the same compile an .obj goes through.

#include <gtest/gtest.h>

#include "../editor/GltfImporter.h"
#include "../editor/ObjImporter.h"
#include "../MeshAsset.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace Deki3D;
namespace fs = std::filesystem;

namespace
{

void Append(std::vector<unsigned char>& out, const void* data, size_t bytes)
{
    const auto* p = static_cast<const unsigned char*>(data);
    out.insert(out.end(), p, p + bytes);
}

void AppendU32(std::vector<unsigned char>& out, uint32_t value)
{
    Append(out, &value, 4);
}

// One triangle in a .glb, in a node moved 10 along X.
std::vector<unsigned char> TriangleGlb()
{
    std::vector<unsigned char> bin;
    const float positions[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    Append(bin, positions, sizeof(positions));
    const uint16_t indices[3] = { 0, 1, 2 };
    Append(bin, indices, sizeof(indices));
    while (bin.size() % 4)
    {
        bin.push_back(0);
    }

    std::string text = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
                       R"("nodes":[{"mesh":0,"translation":[10,0,0]}],)"
                       R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],)"
                       R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],)"
                       R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
                       R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],)"
                       R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
                       R"({"buffer":0,"byteOffset":36,"byteLength":6}],)"
                       R"("buffers":[{"byteLength":44}]})";
    while (text.size() % 4)
    {
        text.push_back(' ');
    }

    std::vector<unsigned char> glb;
    AppendU32(glb, 0x46546C67);
    AppendU32(glb, 2);
    AppendU32(glb, static_cast<uint32_t>(12 + 8 + text.size() + 8 + bin.size()));
    AppendU32(glb, static_cast<uint32_t>(text.size()));
    AppendU32(glb, 0x4E4F534A);
    Append(glb, text.data(), text.size());
    AppendU32(glb, static_cast<uint32_t>(bin.size()));
    AppendU32(glb, 0x004E4942);
    Append(glb, bin.data(), bin.size());
    return glb;
}

fs::path TempDir()
{
    return fs::temp_directory_path() / "deki_gltf_tests";
}

}  // namespace

TEST(GltfImporter, ABinaryGltfCompilesToAMeshWithItsNodeTransform)
{
    GltfConversion conversion;
    std::string error;
    ASSERT_TRUE(ConvertGltfBytesToObj(TriangleGlb(), "", TempDir().string(), conversion, error)) << error;
    EXPECT_NE(conversion.objText.find("v 10 0 0"), std::string::npos) << conversion.objText;

    std::vector<uint8_t> blob;
    ASSERT_TRUE(CompileObjToMesh(conversion.objText, TempDir().string(), nullptr, blob, error)) << error;
    EXPECT_FALSE(blob.empty());
    std::error_code ec;
    fs::remove_all(TempDir(), ec);
}

TEST(GltfImporter, ANonGltfFileIsAnError)
{
    GltfConversion conversion;
    std::string error;
    const std::string text = "not a model";
    EXPECT_FALSE(ConvertGltfBytesToObj(std::vector<unsigned char>(text.begin(), text.end()), "", TempDir().string(),
                                       conversion, error));
    EXPECT_FALSE(error.empty());
}

TEST(GltfImporter, DamagedFilesFailWithoutCrashing)
{
    const std::vector<unsigned char> valid = TriangleGlb();
    std::mt19937 rng(0x6C7Fu);
    for (int i = 0; i < 3000; ++i)
    {
        std::vector<unsigned char> damaged = valid;
        for (int e = 0; e < 1 + static_cast<int>(rng() % 6); ++e)
        {
            damaged[rng() % damaged.size()] = static_cast<unsigned char>(rng() % 2 ? 0xFF : rng());
        }
        if (rng() % 4 == 0)
        {
            damaged.resize(rng() % damaged.size());
        }
        GltfConversion conversion;
        std::string error;
        ConvertGltfBytesToObj(damaged, "", TempDir().string(), conversion, error);
    }
    std::error_code ec;
    fs::remove_all(TempDir(), ec);
}
