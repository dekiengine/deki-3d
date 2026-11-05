// Tests that a scene's tile size is the platform's unless the scene fixes
// one: what a platform's file sets, what a scene saved before gets, and that
// the picture does not depend on it.

#include <gtest/gtest.h>

#include <deki/SceneMigration.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <vector>

#include "../Mesh3DSettings.h"
#include "../Raster3D.h"
#include "../editor/PlatformTileSize.h"

using namespace Deki3D;

TEST(TileSize, APlatformSceneTakesThePlatformsAndAFixedOneItsOwn)
{
    Mesh3DSettings settings;
    EXPECT_EQ(settings.tiles, Mesh3DTiles::Platform) << "a new scene leaves it to the platform";
    EXPECT_EQ(settings.TileSize(), kPlatformTileSize);
    EXPECT_EQ(kPlatformTileSize, 128) << "the editor, which these tests are built as, previews at a desktop's";
    settings.tileSize = 48;
    EXPECT_EQ(settings.TileSize(), kPlatformTileSize) << "the number is for Fixed";
    settings.tiles = Mesh3DTiles::Fixed;
    EXPECT_EQ(settings.TileSize(), 48);
}

TEST(TileSize, APlatformFileSetsItsOwnWithADefine)
{
    DekiEditor::PlatformConfig board;
    board.defines = { "ESP32" };
    EXPECT_EQ(PlatformTileSize(board), 32) << "none named: a microcontroller's 32";
    EXPECT_EQ(PlatformTileSize(board), kUnnamedPlatformTileSize);

    DekiEditor::PlatformConfig handheld;
    handheld.defines = { "SIMULATOR", "DEKI_3D_TILE_SIZE=64" };
    EXPECT_EQ(PlatformTileSize(handheld), 64);

    for (const char* wrong : { "DEKI_3D_TILE_SIZE", "DEKI_3D_TILE_SIZE=", "DEKI_3D_TILE_SIZE=big",
                               "DEKI_3D_TILE_SIZE=64px", "DEKI_3D_TILE_SIZE=4", "DEKI_3D_TILE_SIZE=1024" })
    {
        DekiEditor::PlatformConfig broken;
        broken.defines = { wrong };
        EXPECT_EQ(PlatformTileSize(broken), 0) << wrong << ": its build does not compile";
    }
    DekiEditor::PlatformConfig other;
    other.defines = { "DEKI_3D_TILE_SIZES=64" };
    EXPECT_EQ(PlatformTileSize(other), 32) << "another define";
}

// A scene saved with one tile size for every platform: the old default, 32,
// becomes the platform's, as a new scene's is; another size stays, Fixed.
TEST(TileSize, AnOldSceneWithTheOldDefaultTakesThePlatforms)
{
    nlohmann::json components = nlohmann::json::parse(R"([
        { "type": "Deki3D::Mesh3DSettings", "properties": { "renderer": "Auto", "tileSize": 32 } },
        { "type": "Deki3D::Mesh3DSettings", "properties": { "tileSize": 64 } },
        { "type": "Deki3D::Mesh3DSettings", "properties": { "fillThreads": 2 } },
        { "type": "Deki3D::Mesh3DSettings", "properties": { "tiles": "Fixed", "tileSize": 32 } },
        { "type": "Deki3D::MeshComponent", "properties": { "tileSize": 64 } }
    ])");
    Deki::RunComponentsMigrations(components);
    EXPECT_EQ(components[0]["properties"].at("tiles"), "Platform");
    EXPECT_EQ(components[0]["properties"].at("tileSize"), 32) << "kept, for a switch to Fixed";
    EXPECT_EQ(components[1]["properties"].at("tiles"), "Fixed");
    EXPECT_EQ(components[1]["properties"].at("tileSize"), 64);
    EXPECT_EQ(components[2]["properties"].at("tiles"), "Platform") << "the old default where none was saved";
    EXPECT_EQ(components[3]["properties"].at("tiles"), "Fixed") << "saved since, and fixed at 32 on purpose";
    EXPECT_FALSE(components[4]["properties"].contains("tiles")) << "another component";

    // Saved again, it is left alone.
    nlohmann::json again = components;
    Deki::RunComponentsMigrations(again);
    EXPECT_EQ(again, components);
}

// The tile size changes the speed only: a lit, overlapping scene comes out
// the same at the sizes the platforms use.
TEST(TileSize, ThePictureIsTheSameAtAnySize)
{
    constexpr int kWidth = 150, kHeight = 100;  // not a multiple of the tiles
    std::vector<VertexPNTC> vertices;
    std::vector<uint16_t> indices;
    for (int q = 0; q < 24; ++q)
    {
        const float x = -1.2f + 0.11f * q, y = -0.8f + 0.07f * (q % 7), z = -0.5f + 0.04f * ((q * 5) % 11);
        const uint32_t colour = 0xFF000000u | ((q * 40u) & 0xFF) << 16 | ((255u - q * 9u) & 0xFF) << 8 | 0x60u;
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        const Deki::Vector3 n(0.2f, 0.3f, 1.0f);
        vertices.push_back({ Deki::Vector3(x, y, z), n, 0.0f, 0.0f, colour });
        vertices.push_back({ Deki::Vector3(x + 0.9f, y + 0.1f, z + 0.3f), n, 0.0f, 0.0f, colour });
        vertices.push_back({ Deki::Vector3(x + 0.3f, y + 0.8f, z - 0.2f), n, 0.0f, 0.0f, colour });
        for (uint16_t i : { 0, 1, 2 })
        {
            indices.push_back(static_cast<uint16_t>(base + i));
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
    Material3D material;
    material.shading = ShadingModel::VertexLit;
    material.doubleSided = true;

    auto draw = [&](int tile)
    {
        std::vector<uint8_t> fb(Deki::FrameBufferBytes(Deki::ColorFormat::ARGB8888, kWidth, kHeight), 0);
        RasterConfig config;
        config.tileSize = tile;
        Raster3D raster;
        raster.BeginFrame(fb.data(), kWidth, kHeight, Deki::ColorFormat::ARGB8888, config);
        raster.DrawMesh(mesh, &material, 1, Deki::Mat4::Ortho(-1.5f, 1.5f, -1, 1, -2, 2), Deki::Mat4::Identity());
        raster.EndFrame();
        return fb;
    };
    const std::vector<uint8_t> at32 = draw(32);
    EXPECT_NE(at32, std::vector<uint8_t>(at32.size(), 0)) << "it drew";
    for (int tile : { 8, 64, 128, 512 })
    {
        EXPECT_EQ(draw(tile), at32) << tile << "-pixel tiles";
    }
}
