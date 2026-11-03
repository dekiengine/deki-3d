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

#include "MeshShaderUse.h"

#include "deki-rendering/editor/ShaderUses.h"

namespace Deki3D
{

const char* const kMeshShaderHead = R"(
uniform float4x4 uMvp;
uniform float4x4 uNormalMatrix;  // rotation only, as in the software rasteriser
uniform float3 uLightDir;        // normalised, the way the light travels
uniform float uAmbient;
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

// What a shader's Shade() is given for each pixel.
struct Fragment
{
    float4 base;    // what the shading model draws: albedo times the light
    float4 albedo;  // before the light: the texel, or vertex colour times tint
    float2 uv;
    float3 normal;  // world space: the vertex normal, or the face's when flat
    float light;    // the shading model's light, 0 to 1
    float2 pixel;   // window position, bottom left (0, 0)
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
    [[vk::location(1)]] float light : TEXCOORD0;
    [[vk::location(2)]] float2 uvw : TEXCOORD1;
    [[vk::location(3)]] float w : TEXCOORD2;
    [[vk::location(4)]] float3 facePos : TEXCOORD3;
    [[vk::location(5)]] float3 normal : TEXCOORD4;
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
    o.light = 1.0;
    if (uShading == 1 && uHasNormals != 0)
    {
        float3 n = o.normal;
        float len = length(n);
        if (len > 0.0)
        {
            n /= len;
        }
        o.light = uAmbient + max(-dot(n, uLightDir), 0.0) * (1.0 - uAmbient);
    }
    // For the flat model's face normal, from screen-space derivatives.
    o.facePos = mul((float3x3)uNormalMatrix, position);

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
    float3 face = cross(ddx(i.facePos), ddy(i.facePos));
    float faceLen = length(face);
    face = faceLen > 0.0 ? face / faceLen : float3(0.0, 0.0, 1.0);
    if (!frontFacing)
    {
        face = -face;
    }

    float light = i.light;
    if (uShading == 0)
    {
        light = 1.0;
    }
    else if (uShading == 2)
    {
        light = uAmbient + max(-dot(face, uLightDir), 0.0) * (1.0 - uAmbient);
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
