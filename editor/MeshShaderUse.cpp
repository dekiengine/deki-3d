// Editor-only translation unit (a firmware build compiles nothing from it).
#ifdef DEKI_EDITOR

// The GPU meshes' shader use (deki-rendering's ShaderUses.h), and with no
// hooks the built-in program every material without a shader draws with
// (GLMesh3DBuiltinShader.inl, generated from this; see
// tests/GLBackendTests.cpp). The shading model, the texture kind and the
// alpha test are uniforms, so a batch of materials never switches programs.
//
// A file's hooks, each optional:
//   float3 Displace(float3 position, float3 normal)   moves a vertex
//   float4 Shade(Fragment f)                          colours a pixel; f is
//                                                     what the shading model
//                                                     would draw, and why
// and LightAt(position, normal), the scene's light at a world position,
// shadows and all, for a Shade that lights per pixel.

#include "MeshShaderUse.h"

#include "deki-rendering/editor/ShaderUses.h"

namespace Deki3D
{

const char* const kMeshShaderHead = R"(
uniform float4x4 uMvp;
uniform float4x4 uModel;         // object to world
uniform float4x4 uNormalMatrix;  // rotation only, as in the software rasteriser
// The scene's light (Mesh3DBackend.h's Light3D): the ambient, and up to 8
// lights, each its position and kind (0 directional, 1 point, 2 spot), the
// way it travels and its range, its colour times its intensity, and a spot's
// cone (the cosines of its edge and of where it starts to fade).
uniform float3 uAmbient;
uniform int uLightCount;
uniform float4 uLightPosition[8];
uniform float4 uLightDirection[8];
uniform float4 uLightColor[8];
uniform float4 uLightCone[8];    // z: the shadow map it casts into, 1 or 2, or 0
// The shadow maps (Mesh3DBackend.h's ShadowMap): world to each light's clip
// space, and each one's depth bias.
uniform float4x4 uShadowMatrix0;
uniform float4x4 uShadowMatrix1;
uniform float2 uShadowBias;
uniform int uShading;            // ShadingModel: 0 unlit, 1 vertex lit, 2 flat
uniform int uHasNormals;
uniform int uAffine;             // texturing without perspective correction
uniform int uSnap;               // projected vertices on whole pixels
uniform float2 uViewport;
uniform float uTime;             // seconds, for a shader that moves
uniform float3 uParams;          // the component's shader parameters
uniform float4 uColor;           // the component's shader colour
uniform int uTexMode;            // 0 none, 1 colour texels, 2 coverage only (ALPHA8)
uniform int uAlphaTest;          // discard the texels the texture calls holes
uniform float4 uTint;

Texture2D uTexture;
SamplerState uTextureSampler;
Texture2D uShadowMap0;
SamplerComparisonState uShadowMap0Sampler;
Texture2D uShadowMap1;
SamplerComparisonState uShadowMap1Sampler;

// How much of shadow map `map`'s light (1 or 2) reaches `position`: 0 in its
// shadow, 1 in its light, between at a shadow's edge (the depth compare
// filtered over the four nearest texels). Outside the map, all of it.
float ShadowAt(int map, float3 position)
{
    const float4 p = mul(map == 1 ? uShadowMatrix0 : uShadowMatrix1, float4(position, 1.0));
    if (p.w <= 0.0)
    {
        return 1.0;
    }
    const float3 ndc = p.xyz / p.w;
    const float2 uv = ndc.xy * 0.5 + 0.5;
    const float depth = ndc.z * 0.5 + 0.5 - (map == 1 ? uShadowBias.x : uShadowBias.y);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || depth >= 1.0)
    {
        return 1.0;
    }
    return map == 1 ? uShadowMap0.SampleCmpLevelZero(uShadowMap0Sampler, uv, depth)
                    : uShadowMap1.SampleCmpLevelZero(uShadowMap1Sampler, uv, depth);
}

// The light reaching a surface at `position` facing `normal` (unit), world
// space, before shadows: the ambient, plus each light by how near it is and
// how much the surface faces it (Mesh3DBackend.cpp's LightAt). In `open`
// what no shadow map can block, in `shaded0` and `shaded1` what maps 1 and 2
// can.
void LightParts(float3 position, float3 normal, out float3 open, out float3 shaded0, out float3 shaded1)
{
    open = uAmbient;
    shaded0 = float3(0.0, 0.0, 0.0);
    shaded1 = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < uLightCount; i++)
    {
        const float4 at = uLightPosition[i];
        const float3 travels = uLightDirection[i].xyz;
        float3 toward = -travels;
        float share = 1.0;
        if (at.w > 0.5)
        {
            toward = at.xyz - position;
            const float range = uLightDirection[i].w;
            const float d2 = dot(toward, toward);
            const float t = 1.0 - d2 / (range * range);
            if (t <= 0.0)
            {
                continue;
            }
            share = t * t;
            if (d2 > 0.0)
            {
                toward *= rsqrt(d2);
            }
            if (at.w > 1.5)
            {
                const float4 cone = uLightCone[i];
                const float s = saturate((-dot(toward, travels) - cone.x) / (cone.y - cone.x));
                share *= s * s * (3.0 - 2.0 * s);
            }
        }
        const float3 light = uLightColor[i].rgb * (max(dot(normal, toward), 0.0) * share);
        const float map = uLightCone[i].z;
        if (map > 1.5)
        {
            shaded1 += light;
        }
        else if (map > 0.5)
        {
            shaded0 += light;
        }
        else
        {
            open += light;
        }
    }
}

// The parts put together, through the shadow maps at `position`: at most
// twice a colour.
float3 Shadowed(float3 open, float3 shaded0, float3 shaded1, float3 position)
{
    float3 light = open;
    if (any(shaded0 > 0.0))
    {
        light += shaded0 * ShadowAt(1, position);
    }
    if (any(shaded1 > 0.0))
    {
        light += shaded1 * ShadowAt(2, position);
    }
    return min(light, 2.0);
}

// The light reaching a surface at `position` facing `normal`, shadows and all.
float3 LightAt(float3 position, float3 normal)
{
    float3 open, shaded0, shaded1;
    LightParts(position, normal, open, shaded0, shaded1);
    return Shadowed(open, shaded0, shaded1, position);
}

// What a shader's Shade() is given for each pixel.
struct Fragment
{
    float4 base;      // what the shading model draws: albedo times the light
    float4 albedo;    // before the light: the texel, or vertex colour times tint
    float2 uv;
    float3 normal;    // world space: the vertex normal, or the face's when flat
    float3 light;     // the shading model's light per channel, 0 to 2 (1 the colour as it is)
    float3 position;  // world space
    float2 pixel;     // window position, bottom left (0, 0)
};
)";

const char* const kMeshShaderTail = R"(
struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float4 color : COLOR0;
};

struct Varyings
{
    float4 clip : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float3 light : TEXCOORD0;
    [[vk::location(2)]] float2 uvw : TEXCOORD1;
    [[vk::location(3)]] float w : TEXCOORD2;
    [[vk::location(4)]] float3 position : TEXCOORD3;  // world space
    [[vk::location(5)]] float3 normal : TEXCOORD4;
    // A vertex lit mesh's light from each shadow map's light, shadowed per
    // pixel; `light` holds the rest.
    [[vk::location(6)]] float3 shaded0 : TEXCOORD5;
    [[vk::location(7)]] float3 shaded1 : TEXCOORD6;
};

Varyings VertexMain(VertexIn v)
{
    float3 position = v.position;
#ifdef DEKI_DISPLACE
    position = Displace(v.position, v.normal);
#endif
    float4 clip = mul(uMvp, float4(position, 1.0));
    if (uSnap != 0 && clip.w > 0.0)
    {
        float2 pixel = floor((clip.xy / clip.w * 0.5 + 0.5) * uViewport + 0.5);
        clip.xy = (pixel / uViewport * 2.0 - 1.0) * clip.w;
    }

    Varyings o;
    o.clip = clip;
    o.color = v.color;
    o.normal = mul((float3x3)uNormalMatrix, v.normal);
    o.position = mul(uModel, float4(position, 1.0)).xyz;
    o.light = float3(1.0, 1.0, 1.0);
    o.shaded0 = float3(0.0, 0.0, 0.0);
    o.shaded1 = float3(0.0, 0.0, 0.0);
    if (uShading == 1 && uHasNormals != 0)
    {
        float3 n = o.normal;
        float len = length(n);
        if (len > 0.0)
        {
            n /= len;
        }
        LightParts(o.position, n, o.light, o.shaded0, o.shaded1);
    }

    // Affine texturing: interpolating uv*w and w perspective-correctly, then
    // dividing, leaves uv interpolated linearly across the screen.
    float w = uAffine != 0 ? clip.w : 1.0;
    o.uvw = v.uv * w;
    o.w = w;
    return o;
}

float4 FragmentMain(Varyings i, bool frontFacing : SV_IsFrontFace) : SV_Target0
{
    float4 colour = i.color * uTint;
    float coverage = 1.0;
    float2 uv = i.uvw / i.w;
    if (uTexMode != 0)
    {
        float4 texel = uTexture.Sample(uTextureSampler, uv);
        coverage = texel.a;
        if (uTexMode == 1)
        {
            // The texel alone, as the software rasteriser draws it: vertex
            // colour and tint colour only untextured and coverage-only meshes.
            colour = float4(texel.rgb, 1.0);
        }
    }
    if (uAlphaTest != 0 && coverage < 0.5)
    {
        discard;
    }

    // The face normal, from screen-space derivatives: they face the viewer,
    // and a back face's normal does not.
    float3 face = cross(ddx(i.position), ddy(i.position));
    float faceLen = length(face);
    face = faceLen > 0.0 ? face / faceLen : float3(0.0, 0.0, 1.0);
    if (!frontFacing)
    {
        face = -face;
    }

    float3 light = i.light;
    if (uShading == 0)
    {
        light = float3(1.0, 1.0, 1.0);
    }
    else if (uShading == 1)
    {
        light = Shadowed(i.light, i.shaded0, i.shaded1, i.position);
    }
    else if (uShading == 2)
    {
        light = LightAt(i.position, face);
    }
    float4 base = float4(colour.rgb * light, 1.0);

#ifdef DEKI_SHADE
    Fragment f;
    f.base = base;
    f.albedo = float4(colour.rgb, coverage);
    f.uv = uv;
    float vertexLen = length(i.normal);
    f.normal = (uShading == 2 || uHasNormals == 0 || vertexLen == 0.0) ? face : i.normal / vertexLen;
    f.light = light;
    f.position = i.position;
    f.pixel = i.clip.xy;
    // Opaque: the meshes cover what is below them, as in the rasteriser.
    return float4(Shade(f).rgb, 1.0);
#else
    return base;
#endif
}
)";

DekiRendering::ShaderUse MeshShaderUse()
{
    DekiRendering::ShaderUse use;
    use.name = "mesh";
    use.hooks = { { "Displace", "DEKI_DISPLACE", false }, { "Shade", "DEKI_SHADE", false } };
    use.head = kMeshShaderHead;
    use.tail = kMeshShaderTail;
    return use;
}

namespace
{
struct MeshShaderUseRegistrar
{
    MeshShaderUseRegistrar() { DekiRendering::RegisterShaderUse(MeshShaderUse()); }
};
MeshShaderUseRegistrar s_MeshShaderUseRegistrar;
}  // namespace

}  // namespace Deki3D

#endif  // DEKI_EDITOR
