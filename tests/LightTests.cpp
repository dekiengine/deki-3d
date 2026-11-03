// The meshes' light: LightAt, the sum both backends light with (falloff, a
// spot's cone, how much a surface faces a light, twice a colour at most);
// several coloured lights drawn on the GPU as the software rasteriser draws
// them; and a scene saved with Mesh3DSettings' one light moved onto Light
// components that light it exactly as before.

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include <deki/SceneMigration.h>
#include <nlohmann/json.hpp>

#include "../Mesh3DBackend.h"

using namespace Deki3D;

namespace
{
Light3D Point(Deki::Vector3 at, float range, Deki::Vector3 color = Deki::Vector3(1, 1, 1))
{
    Light3D l;
    l.kind = Light3D::Kind::Point;
    l.position = at;
    l.range = range;
    l.color = color;
    return l;
}

Light3D Sun(Deki::Vector3 travels, Deki::Vector3 color)
{
    Light3D l;
    l.kind = Light3D::Kind::Directional;
    l.direction = travels;
    l.direction.Normalize();
    l.color = color;
    return l;
}
}  // namespace

TEST(Light3D, APointFadesToNothingAtItsRange)
{
    const Light3D l = Point(Deki::Vector3(0, 2, 0), 4.0f);
    const Deki::Vector3 up(0, 1, 0);
    const Deki::Vector3 none(0, 0, 0);
    const float under = LightAt(none, &l, 1, Deki::Vector3(0, 0, 0), up).x;     // 2 away of 4
    const float further = LightAt(none, &l, 1, Deki::Vector3(0, -1, 0), up).x;  // 3 away
    const float past = LightAt(none, &l, 1, Deki::Vector3(0, -3, 0), up).x;     // 5 away
    EXPECT_NEAR(under, (1 - 4.0f / 16) * (1 - 4.0f / 16), 1e-5f);
    EXPECT_LT(further, under);
    EXPECT_EQ(past, 0.0f);
}

TEST(Light3D, ASurfaceTurnedAwayGetsNoneOfALight)
{
    const Light3D sun = Sun(Deki::Vector3(0, -1, 0), Deki::Vector3(1, 1, 1));
    const Deki::Vector3 ambient(0.2f, 0.2f, 0.2f);
    EXPECT_NEAR(LightAt(ambient, &sun, 1, Deki::Vector3(), Deki::Vector3(0, 1, 0)).y, 1.2f, 1e-5f);
    EXPECT_NEAR(LightAt(ambient, &sun, 1, Deki::Vector3(), Deki::Vector3(0, -1, 0)).y, 0.2f, 1e-5f);
    const float side = std::sqrt(0.5f);
    EXPECT_NEAR(LightAt(ambient, &sun, 1, Deki::Vector3(), Deki::Vector3(side, side, 0)).y, 0.2f + side, 1e-5f);
}

TEST(Light3D, ASpotLightsOnlyInsideItsCone)
{
    Light3D spot = Point(Deki::Vector3(0, 5, 0), 20.0f);
    spot.kind = Light3D::Kind::Spot;
    spot.direction = Deki::Vector3(0, -1, 0);
    spot.cosOuter = std::cos(0.4f);
    spot.cosInner = std::cos(0.3f);
    const Deki::Vector3 up(0, 1, 0), none(0, 0, 0);
    EXPECT_GT(LightAt(none, &spot, 1, Deki::Vector3(0, 0, 0), up).x, 0.5f) << "right under it";
    EXPECT_EQ(LightAt(none, &spot, 1, Deki::Vector3(4, 0, 0), up).x, 0.0f) << "well outside the cone";
    const float edge = LightAt(none, &spot, 1, Deki::Vector3(5 * std::tan(0.35f), 0, 0), up).x;
    const float inside = LightAt(none, &spot, 1, Deki::Vector3(5 * std::tan(0.2f), 0, 0), up).x;
    EXPECT_GT(edge, 0.0f);
    EXPECT_LT(edge, inside) << "fading at its edge";
}

TEST(Light3D, LightsAddUpInColourToTwiceAColour)
{
    const Light3D lights[] = { Sun(Deki::Vector3(0, -1, 0), Deki::Vector3(1, 0, 0)),
                               Sun(Deki::Vector3(0, -1, 0), Deki::Vector3(0.5f, 0.5f, 0)),
                               Sun(Deki::Vector3(0, -1, 0), Deki::Vector3(2, 0, 0.25f)) };
    const Deki::Vector3 l = LightAt(Deki::Vector3(0, 0, 0.1f), lights, 3, Deki::Vector3(), Deki::Vector3(0, 1, 0));
    EXPECT_FLOAT_EQ(l.x, 2.0f) << "capped";
    EXPECT_FLOAT_EQ(l.y, 0.5f);
    EXPECT_FLOAT_EQ(l.z, 0.35f);
}

TEST(Light3D, TheOldSceneLightBecomesLightsThatLightAsItDid)
{
    nlohmann::json components = nlohmann::json::parse(R"([
        { "type": "Deki3D::Mesh3DSettings",
          "properties": { "ambient": 0.3, "lightYaw": 40.0, "lightPitch": 20.0, "tileSize": 32 } }
    ])");
    Deki::RunComponentsMigrations(components);
    ASSERT_EQ(components.size(), 3u);
    const nlohmann::json& settings = components[0]["properties"];
    EXPECT_FALSE(settings.contains("ambient"));
    EXPECT_FALSE(settings.contains("lightYaw"));
    EXPECT_FALSE(settings.contains("lightPitch"));
    EXPECT_EQ(settings.at("tileSize"), 32) << "the rest stays";

    const nlohmann::json& ambient = components[1];
    EXPECT_EQ(ambient.at("type"), "DekiRendering::Light");
    EXPECT_EQ(ambient["properties"].at("type"), "Ambient");
    const float a = ambient["properties"].at("intensity").get<float>();
    EXPECT_NEAR(a, 0.3f, 1e-6f);
    const nlohmann::json& sun = components[2]["properties"];
    EXPECT_EQ(sun.at("type"), "Directional");
    EXPECT_NEAR(sun.at("intensity").get<float>(), 0.7f, 1e-6f);
    EXPECT_NEAR(sun.at("yaw").get<float>(), 40.0f, 1e-6f);
    EXPECT_NEAR(sun.at("pitch").get<float>(), 20.0f, 1e-6f);

    // ambient + facing x (1 - ambient), as the one light lit a face.
    const Light3D light = Sun(Deki::Vector3(0.3f, -0.5f, -0.8f), Deki::Vector3(0.7f, 0.7f, 0.7f));
    for (const Deki::Vector3& n : { Deki::Vector3(0, 1, 0), Deki::Vector3(0.6f, 0.8f, 0), Deki::Vector3(0, 0, -1) })
    {
        const float facing = std::max(-n.Dot(light.direction), 0.0f);
        EXPECT_NEAR(LightAt(Deki::Vector3(a, a, a), &light, 1, Deki::Vector3(), n).x, a + facing * (1 - a), 1e-5f);
    }

    // Saved again, it is left alone.
    nlohmann::json again = components;
    Deki::RunComponentsMigrations(again);
    EXPECT_EQ(again, components);
}
