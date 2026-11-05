#include "Raster3D.h"
#include "Math3D.h"
#include "deki-rendering/PixelFormat.h"

#include <deki/LogSystem.h>

#include <algorithm>
#include <chrono>
#if defined(__XTENSA__)
#include <xtensa/config/core-isa.h>  // XCHAL_HAVE_FP_RECIP
#endif
#include <cmath>
#include <cstring>
#include <thread>

#if defined(ESP_PLATFORM)
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace Deki3D
{
namespace
{

// Screen coordinates are 28.4: four fractional bits give the edge functions
// sub-pixel precision without the products leaving 64 bits.
constexpr int kSubBits = 4;
constexpr int kSubScale = 1 << kSubBits;

// Depth is 24.8 over a 16-bit depth range. 16 fractional bits would push the
// accumulator past INT32_MAX at the far plane; eight is already more than the
// depth buffer can resolve.
constexpr int kDepthFrac = 8;
constexpr int32_t kDepthMax = 0xFFFF;

// Texture coordinates are 16.16 texels. A 1024-texel axis leaves plenty of
// headroom before the accumulator overflows, and wrapping is a mask.
constexpr int kUvFrac = 16;

inline Deki::Vector3 ReadVec3(const void* vertex, int16_t offset)
{
    const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(vertex) + offset);
    return Deki::Vector3(f[0], f[1], f[2]);
}

inline void ReadUV(const void* vertex, int16_t offset, float& u, float& v)
{
    const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(vertex) + offset);
    u = f[0];
    v = f[1];
}

inline uint32_t ReadColor(const void* vertex, int16_t offset)
{
    uint32_t c;
    std::memcpy(&c, static_cast<const uint8_t*>(vertex) + offset, sizeof(c));
    return c;
}

/// 0xAABBGGRR down to RGB565.
inline uint16_t To565(uint32_t rgba)
{
    const uint32_t r = (rgba >> 0) & 0xFF;
    const uint32_t g = (rgba >> 8) & 0xFF;
    const uint32_t b = (rgba >> 16) & 0xFF;
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/// Scales an RGB565 pixel by a shade per channel in 0..64 (32 is the colour
/// as it is), kept within each channel.
inline uint16_t Shade565RGB(uint16_t c, uint32_t sr, uint32_t sg, uint32_t sb)
{
    const uint32_t r = std::min<uint32_t>(((c >> 11) * sr) >> 5, 31);
    const uint32_t g = std::min<uint32_t>((((c >> 5) & 0x3Fu) * sg) >> 5, 63);
    const uint32_t b = std::min<uint32_t>(((c & 0x1Fu) * sb) >> 5, 31);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

/// Scales an RGB565 pixel by shade in 0..32 with one multiply: green moved up
/// to bits 21..26 leaves every channel the shade's 5 bits to grow into below
/// the next, so each comes out as its own multiply would make it (checked for
/// every colour and shade), with one mask the span loop can keep.
inline uint16_t Shade565(uint16_t c, uint32_t shade)
{
    uint32_t x = (static_cast<uint32_t>(c) | (static_cast<uint32_t>(c) << 16)) & 0x07E0F81Fu;
    x = (x * shade >> 5) & 0x07E0F81Fu;
    return static_cast<uint16_t>(x | (x >> 16));
}

/// Plane gradients of a screen-linear attribute across a triangle. invW, z,
/// u/w and v/w are all linear in screen space, which is why the span loop can
/// step them with adds.
struct Gradient
{
    float dx = 0.0f;
    float dy = 0.0f;
    float at0 = 0.0f;
};

inline Gradient MakeGradient(float f0, float f1, float f2, float x0, float y0, float x1, float y1, float x2, float y2,
                             float invArea)
{
    Gradient g;
    g.at0 = f0;
    g.dx = ((f1 - f0) * (y2 - y0) - (f2 - f0) * (y1 - y0)) * invArea;
    g.dy = ((f2 - f0) * (x1 - x0) - (f1 - f0) * (x2 - x0)) * invArea;
    return g;
}

// floor and ceil by a cast, which truncates toward zero, and a correction:
// the same results as std::floor and std::ceil for anything an int holds, and
// a few instructions, where those are calls on some cores (Xtensa's).
inline int FloorToInt(float f)
{
    const int i = static_cast<int>(f);
    return i - (f < static_cast<float>(i) ? 1 : 0);
}

inline int CeilToInt(float f)
{
    const int i = static_cast<int>(f);
    return i + (f > static_cast<float>(i) ? 1 : 0);
}

// 1 / x, for x not 0. On an Xtensa FPU (the ESP32-S3's) a divide is a
// library call, costing a span about 76 cycles with its register-window
// spills; there it is the FPU's reciprocal estimate and two Newton steps,
// inline, each doubling its correct bits to the float's 24, within an ulp of
// the divide. Elsewhere the divide itself.
inline float Reciprocal(float x)
{
#if defined(__XTENSA__) && XCHAL_HAVE_FP_RECIP
    float r;
    __asm__("recip0.s %0, %1" : "=f"(r) : "f"(x));
    r = r + r * (1.0f - x * r);
    r = r + r * (1.0f - x * r);
    return r;
#else
    return 1.0f / x;
#endif
}

inline float Eval(const Gradient& g, float dxFromV0, float dyFromV0)
{
    return g.at0 + g.dx * dxFromV0 + g.dy * dyFromV0;
}

/// True when the mesh's bounding box is wholly outside the view.
///
/// Moves the eight corners to clip space and checks whether all of them fail
/// one of the six planes. Conservative: a box straddling two planes without
/// touching the frustum is kept, but nothing visible is ever culled. Eight
/// transforms, against the thousands the mesh would cost.
bool BoxOutsideFrustum(const Deki::Vector3& lo, const Deki::Vector3& hi, const Deki::Mat4& mvp)
{
    // No bounds is min == max == 0; culling on that would hide the mesh
    // whenever the origin left the view.
    if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z)
    {
        return false;
    }
    if (lo.x == hi.x && lo.y == hi.y && lo.z == hi.z)
    {
        return false;
    }

    int outside[6] = { 0, 0, 0, 0, 0, 0 };
    for (int corner = 0; corner < 8; ++corner)
    {
        const Deki::Vector3 point((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z);
        float w = 1.0f;
        const Deki::Vector3 clip = Deki3D::TransformPoint(mvp, point, w);
        if (clip.x < -w)
        {
            ++outside[0];
        }
        if (clip.x > w)
        {
            ++outside[1];
        }
        if (clip.y < -w)
        {
            ++outside[2];
        }
        if (clip.y > w)
        {
            ++outside[3];
        }
        if (clip.z < -w)
        {
            ++outside[4];
        }
        if (clip.z > w)
        {
            ++outside[5];
        }
    }
    for (int plane = 0; plane < 6; ++plane)
    {
        if (outside[plane] == 8)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The inner loop, one instance per texture layout and framebuffer format.
// ---------------------------------------------------------------------------

/// What a span needs from its triangle, fixed across the span.
struct SpanArgs
{
    uint8_t* fbRow;      // start of this scanline in the framebuffer
    uint16_t* depthRow;  // indexed by x, like fbRow
    int x0, x1;          // inclusive
    int32_t u, v, z;     // at x0, fixed point
    int32_t du, dv, dz;
    const Texture3D* tex;
    bool alphaTest;
    bool lit;
    // The light per channel, 0..64 (32 is the colour as it is); grey when
    // the three are one value no brighter than the colour, which Shade565
    // does with two multiplies.
    uint32_t shadeR, shadeG, shadeB;
    bool grey;
    uint16_t flat565;  // untextured colour, as RGB565 and as 8-bit channels
    uint8_t flatR, flatG, flatB;
    int uMask, vMask, wShift;
    bool depthTest, depthWrite;
};

/// The texture side of a span: none (the flat colour), an RGB565 palette, or
/// texels of one TexelFormat.
enum class TexKind
{
    Flat,
    Palette,
    RGB565,
    RGB565A8,
    RGB888,
    RGBA8888,
    ALPHA8
};

/// Scales 8-bit channels by a shade each in 0..64, as Shade565RGB does for a
/// 565 pixel.
inline void Shade8(uint8_t& r, uint8_t& g, uint8_t& b, uint32_t sr, uint32_t sg, uint32_t sb)
{
    r = static_cast<uint8_t>(std::min<uint32_t>((r * sr) >> 5, 255));
    g = static_cast<uint8_t>(std::min<uint32_t>((g * sg) >> 5, 255));
    b = static_cast<uint8_t>(std::min<uint32_t>((b * sb) >> 5, 255));
}

/// Fills one span and returns how many pixels it wrote: any framebuffer
/// format and texture, through 8-bit channels and the 2D blitter's pixel
/// helpers. An RGB565 framebuffer with a flat, palette or RGB565 texture
/// takes FillSpan565 instead. With alphaTest, a texel whose alpha is below
/// half is a hole.
template <TexKind K, Deki::ColorFormat F>
uint32_t FillSpan(const SpanArgs& a)
{
    int32_t u = a.u, v = a.v, z = a.z;
    uint32_t written = 0;
    for (int x = a.x0; x <= a.x1; ++x)
    {
        const int32_t depth = z >> kDepthFrac;
        if (!a.depthTest || depth < static_cast<int32_t>(a.depthRow[x]))
        {
            bool write = true;
            int texel = 0;
            if constexpr (K != TexKind::Flat)
            {
                const int tu = (u >> kUvFrac) & a.uMask;
                const int tv = (v >> kUvFrac) & a.vMask;
                texel = (tv << a.wShift) + tu;
            }

            uint8_t r = a.flatR, g = a.flatG, b = a.flatB, al = 255;
            if constexpr (K == TexKind::Palette)
            {
                const uint8_t idx = a.tex->pixels[texel];
                if (a.alphaTest && idx == 0)
                {
                    write = false;
                }
                else
                {
                    DekiPixel::UnpackRGB565(a.tex->palette[idx], r, g, b);
                }
            }
            else if constexpr (K == TexKind::RGB565)
            {
                const uint16_t c = DekiPixel::FromMemory565(reinterpret_cast<const uint16_t*>(a.tex->pixels)[texel]);
                if (a.alphaTest && c == 0xF81F)
                {
                    write = false;
                }
                else
                {
                    DekiPixel::UnpackRGB565(c, r, g, b);
                }
            }
            else if constexpr (K == TexKind::RGB565A8)
            {
                DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGB565A8>(a.tex->pixels + texel * 3, true, r, g, b, al);
            }
            else if constexpr (K == TexKind::RGB888)
            {
                DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGB888>(a.tex->pixels + texel * 3, false, r, g, b, al);
            }
            else if constexpr (K == TexKind::RGBA8888)
            {
                DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGBA8888>(a.tex->pixels + texel * 4, true, r, g, b, al);
            }
            else if constexpr (K == TexKind::ALPHA8)
            {
                // Coverage only: the colour is the material's.
                al = a.tex->pixels[texel];
            }
            if (a.alphaTest && al < 128)
            {
                write = false;
            }
            if (write)
            {
                if (a.lit)
                {
                    Shade8(r, g, b, a.shadeR, a.shadeG, a.shadeB);
                }
                DekiPixel::WriteDstPixel<F>(a.fbRow, static_cast<size_t>(x), r, g, b, 255);
            }

            if (write)
            {
                if (a.depthWrite)
                {
                    a.depthRow[x] = static_cast<uint16_t>(depth);
                }
                ++written;
            }
        }
        if constexpr (K != TexKind::Flat)
        {
            u += a.du;
            v += a.dv;
        }
        z += a.dz;
    }
    return written;
}

/// An RGB565 framebuffer with a flat, palette or RGB565 texture: 565
/// arithmetic and magenta holes. This path must stay pixel-identical, since
/// the RGB565 device output depends on it. The arithmetic is on 565 values;
/// texels and the framebuffer hold them in memory's byte order
/// (DekiPixel::ToMemory565), so they are converted on the way in and out,
/// which costs nothing unless the board is big-endian.
///
/// The span's settings are read once into locals, so the loop keeps them in
/// registers: through the framebuffer and depth stores the compiler cannot
/// tell they do not change, and it read them again for every pixel. The
/// shading (`Shade`: none, grey, per channel), the cutout, and the depth test
/// and write both on (`Depth`, as they are but for an effect) are chosen
/// once per triangle, not tested per pixel. It counts the pixels it skips,
/// which are few, rather than those it writes: the count is off the path of
/// a pixel written, where the loop has no register to spare for it.
template <TexKind K, int Shade, bool Cutout, bool Depth>
uint32_t FillSpan565(const SpanArgs& a)
{
    uint16_t* const fb = reinterpret_cast<uint16_t*>(a.fbRow);
    uint16_t* const depthRow = a.depthRow;
    const bool depthTest = Depth || a.depthTest;
    const bool depthWrite = Depth || a.depthWrite;
    const uint8_t* const pixels = K != TexKind::Flat ? a.tex->pixels : nullptr;
    const uint16_t* const palette = K == TexKind::Palette ? a.tex->palette : nullptr;
    const uint16_t flat = a.flat565;
    const int uMask = a.uMask, vMask = a.vMask, wShift = a.wShift;
    const int32_t du = a.du, dv = a.dv, dz = a.dz;
    const uint32_t shadeR = a.shadeR, shadeG = a.shadeG, shadeB = a.shadeB;
    const int x1 = a.x1;
    int32_t u = a.u, v = a.v, z = a.z;
    uint32_t skipped = 0;
    for (int x = a.x0; x <= x1; ++x)
    {
        const int32_t depth = z >> kDepthFrac;
        if (depthTest && depth >= static_cast<int32_t>(depthRow[x]))
        {
            ++skipped;
        }
        else
        {
            uint16_t color = flat;
            bool write = true;
            if constexpr (K != TexKind::Flat)
            {
                const int texel = (((v >> kUvFrac) & vMask) << wShift) + ((u >> kUvFrac) & uMask);
                if constexpr (K == TexKind::Palette)
                {
                    const uint8_t idx = pixels[texel];
                    if (Cutout && idx == 0)
                    {
                        write = false;
                    }
                    else
                    {
                        color = palette[idx];
                    }
                }
                else
                {
                    color = DekiPixel::FromMemory565(reinterpret_cast<const uint16_t*>(pixels)[texel]);
                    if (Cutout && color == 0xF81F)  // magenta is the hole
                    {
                        write = false;
                    }
                }
            }
            if (write)
            {
                if constexpr (Shade == 1)
                {
                    color = Shade565(color, shadeR);
                }
                else if constexpr (Shade == 2)
                {
                    color = Shade565RGB(color, shadeR, shadeG, shadeB);
                }
                fb[x] = DekiPixel::ToMemory565(color);
                if (depthWrite)
                {
                    depthRow[x] = static_cast<uint16_t>(depth);
                }
            }
            else
            {
                ++skipped;
            }
        }
        if constexpr (K != TexKind::Flat)
        {
            u += du;
            v += dv;
        }
        z += dz;
    }
    return a.x1 >= a.x0 ? static_cast<uint32_t>(a.x1 - a.x0 + 1) - skipped : 0;
}

/// The loop that fills a span, chosen once per triangle: its texture kind,
/// the framebuffer's format, and for an RGB565 framebuffer its shading and
/// cutout, which `a` already holds.
using SpanFn = uint32_t (*)(const SpanArgs&);

template <TexKind K, bool Cutout>
SpanFn Span565For(const SpanArgs& a)
{
    const bool depth = a.depthTest && a.depthWrite;
    if (!a.lit)
    {
        return depth ? &FillSpan565<K, 0, Cutout, true> : &FillSpan565<K, 0, Cutout, false>;
    }
    if (a.grey)
    {
        return depth ? &FillSpan565<K, 1, Cutout, true> : &FillSpan565<K, 1, Cutout, false>;
    }
    return depth ? &FillSpan565<K, 2, Cutout, true> : &FillSpan565<K, 2, Cutout, false>;
}

template <TexKind K>
SpanFn SpanFor(Deki::ColorFormat format, const SpanArgs& a)
{
    if constexpr (K == TexKind::Flat || K == TexKind::Palette || K == TexKind::RGB565)
    {
        if (format == Deki::ColorFormat::RGB565)
        {
            return a.alphaTest ? Span565For<K, true>(a) : Span565For<K, false>(a);
        }
    }
    switch (format)
    {
        case Deki::ColorFormat::RGB565: return &FillSpan<K, Deki::ColorFormat::RGB565>;
        case Deki::ColorFormat::RGB888: return &FillSpan<K, Deki::ColorFormat::RGB888>;
        case Deki::ColorFormat::ARGB8888: return &FillSpan<K, Deki::ColorFormat::ARGB8888>;
        case Deki::ColorFormat::RGB565A8: return &FillSpan<K, Deki::ColorFormat::RGB565A8>;
    }
    return nullptr;
}

SpanFn ChooseSpan(TexKind kind, Deki::ColorFormat format, const SpanArgs& a)
{
    switch (kind)
    {
        case TexKind::Flat: return SpanFor<TexKind::Flat>(format, a);
        case TexKind::Palette: return SpanFor<TexKind::Palette>(format, a);
        case TexKind::RGB565: return SpanFor<TexKind::RGB565>(format, a);
        case TexKind::RGB565A8: return SpanFor<TexKind::RGB565A8>(format, a);
        case TexKind::RGB888: return SpanFor<TexKind::RGB888>(format, a);
        case TexKind::RGBA8888: return SpanFor<TexKind::RGBA8888>(format, a);
        case TexKind::ALPHA8: return SpanFor<TexKind::ALPHA8>(format, a);
    }
    return nullptr;
}

/// 1 / n for a span's steps, n up to kReciprocals: a span's length is at most
/// its subdivision or its tile's width. Each entry is the division itself, so
/// a span steps exactly as it did dividing.
constexpr int kReciprocals = 256;
const float* Reciprocals()
{
    static float table[kReciprocals + 1] = {};
    static bool filled = false;
    if (!filled)
    {
        for (int n = 1; n <= kReciprocals; ++n)
        {
            table[n] = 1.0f / static_cast<float>(n);
        }
        filled = true;
    }
    return table;
}

TexKind KindOfTexture(const Texture3D* tex)
{
    if (!tex || !tex->Valid())
    {
        return TexKind::Flat;
    }
    if (tex->palette)
    {
        return TexKind::Palette;
    }
    switch (tex->format)
    {
        case TexelFormat::RGB565: return TexKind::RGB565;
        case TexelFormat::RGB565A8: return TexKind::RGB565A8;
        case TexelFormat::RGB888: return TexKind::RGB888;
        case TexelFormat::RGBA8888: return TexKind::RGBA8888;
        case TexelFormat::ALPHA8: return TexKind::ALPHA8;
    }
    return TexKind::Flat;
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame setup
// ---------------------------------------------------------------------------

void Raster3D::BeginFrame(uint8_t* buffer, int32_t width, int32_t height, Deki::ColorFormat format,
                          const RasterConfig& config)
{
    m_Buffer = buffer;
    m_Width = width;
    m_Height = height;
    m_Format = format;
    m_BytesPerPixel = Deki::FrameBufferBytes(format, 1, 1);
    m_HasBackground = false;
    m_Config = config;
    if (m_Config.tileSize < 8)
    {
        m_Config.tileSize = 8;
    }
    if (m_Config.spanSubdivision < 1)
    {
        m_Config.spanSubdivision = 1;
    }

    m_Config.lightCount = std::min(std::max(m_Config.lightCount, 0), RasterConfig::kMaxLights);
    for (int i = 0; i < m_Config.lightCount; ++i)
    {
        Deki::Vector3& d = m_Config.lights[i].direction;
        if (d.LengthSquared() > 0.0f)
        {
            d.Normalize();
        }
    }

    const int tile = m_Config.tileSize;
    m_TilesX = (width + tile - 1) / tile;
    m_TilesY = (height + tile - 1) / tile;

    // Keep the capacity the last frame grew into. Reassigning would free and
    // reallocate one vector per tile every frame, which costs more than the
    // rasterising on light scenes.
    m_BatchCount = 0;
    m_Materials.clear();
    const size_t tileCount = static_cast<size_t>(m_TilesX) * m_TilesY;
    if (m_Bins.size() != tileCount)
    {
        m_Bins.clear();
        m_Bins.resize(tileCount);
    }
    else
    {
        for (RegionArray<uint32_t>& bin : m_Bins)
        {
            bin.Clear();
        }
    }
    if (m_Config.batchTriangles < 1)
    {
        m_Config.batchTriangles = 1;
    }
    m_FrameDepthActive = false;
    m_FrameFull = false;

    // One scratch depth buffer per fill thread, sized once and kept. The
    // calling thread is one of them, so `threads - 1` workers are needed.
    if (m_Config.threadCount > RasterConfig::kMaxThreads)
    {
        static bool s_Warned = false;
        if (!s_Warned)
        {
            s_Warned = true;
            DEKI_LOG_WARNING("Raster3D: %d threads asked for; drawing on %d, the most it shares the work between",
                             m_Config.threadCount, RasterConfig::kMaxThreads);
        }
        m_Config.threadCount = RasterConfig::kMaxThreads;
    }
    const int threads = m_Config.threadCount < 1 ? 1 : m_Config.threadCount;
    const size_t depthCells = static_cast<size_t>(tile) * tile;
    if (static_cast<int>(m_TileDepth.size()) != threads)
    {
        m_TileDepth.assign(static_cast<size_t>(threads), {});
    }
    for (std::vector<uint16_t>& buffer : m_TileDepth)
    {
        if (buffer.size() != depthCells)
        {
            buffer.resize(depthCells);
        }
    }
    // And one geometry lane per thread, each with its own vertex cache.
    if (static_cast<int>(m_Lanes.size()) != threads)
    {
        m_Lanes.resize(static_cast<size_t>(threads));
    }
    for (GeometryLane& lane : m_Lanes)
    {
        lane.tris.Clear();
        lane.staging = false;
        if (lane.cache.size() != static_cast<size_t>(kVertexCacheSize))
        {
            lane.cache.resize(kVertexCacheSize);
        }
    }

    if (static_cast<int>(m_Workers.size()) != threads - 1)
    {
        StartWorkers(threads - 1);
    }

    m_Stats.Reset();
}

// ---------------------------------------------------------------------------
// Geometry: transform, clip, project, bin
// ---------------------------------------------------------------------------

void Raster3D::DrawMesh(const Mesh3D& mesh, const Material3D* materials, uint16_t materialCount, const Deki::Mat4& mvp,
                        const Deki::Mat4& normalMatrix, const Deki::Mat4& model)
{
    if (!mesh.vertices || !mesh.indices || mesh.layout.positionOffset < 0)
    {
        return;
    }

    // Eight transforms to decide whether thousands are needed at all.
    if (BoxOutsideFrustum(mesh.boundsMin, mesh.boundsMax, mvp))
    {
        ++m_Stats.meshesCulled;
        return;
    }

    // Its time, less any batch filled on the way, is the geometry's.
    const auto started = std::chrono::steady_clock::now();
    const uint32_t fillBefore = m_Stats.fillMicros;
    struct Timer
    {
        RasterStats& stats;
        std::chrono::steady_clock::time_point started;
        uint32_t fillBefore;
        ~Timer()
        {
            const auto spent =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                    .count();
            stats.geometryMicros += static_cast<uint32_t>(spent) - (stats.fillMicros - fillBefore);
        }
    } timer{ m_Stats, started, fillBefore };

    PickLights(mesh, model);

    for (uint16_t s = 0; s < mesh.submeshCount; ++s)
    {
        const Submesh3D& sub = mesh.submeshes[s];
        const uint32_t triangles = sub.indexCount / 3;
        // A part out of view, as a whole mesh is: a model of many parts
        // (a building, a level) seen from inside has most of them behind or
        // beside the camera.
        if (sub.hasBounds && BoxOutsideFrustum(sub.boundsMin, sub.boundsMax, mvp))
        {
            ++m_Stats.submeshesCulled;
            m_Stats.trianglesIn += triangles;
            continue;
        }
        // Copy the material now: the span loop reads it in EndFrame, when the
        // caller's material may be gone.
        if (m_Materials.size() >= 0xFFFFu)
        {
            return;
        }
        const uint16_t materialIndex = static_cast<uint16_t>(m_Materials.size());
        m_Materials.push_back((materials && sub.materialIndex < materialCount) ? materials[sub.materialIndex]
                                                                               : Material3D{});
        const Material3D& material = m_Materials.back();
        m_Stats.trianglesIn += triangles;

        MeshPart part;
        part.mesh = &mesh;
        part.mvp = &mvp;
        part.normalMatrix = &normalMatrix;
        part.model = &model;
        part.ambient = m_Config.ambient;
        part.firstIndex = sub.firstIndex;
        part.materialIndex = materialIndex;
        part.shading = material.shading;
        part.cullBack = m_Config.backfaceCull && !material.doubleSided;
        part.vertexLit = material.shading == ShadingModel::VertexLit && mesh.layout.HasNormals();

        // Shared out between the threads where there are several and the
        // part is large enough to be worth waking them, in pieces that fit
        // the batch: a triangle makes at most two once near-clipped, so a
        // piece of half the batch's room never fills it, and the batch fills
        // where it would have on one thread. The rest, and a frame whose
        // batch is nearly full, go triangle by triangle, as on one thread.
        // The same triangles reach the same tiles in the same order either
        // way, so the picture does not depend on the threads.
        for (GeometryLane& lane : m_Lanes)
        {
            lane.EmptyCache();
        }
        uint32_t done = 0;
        while (done < triangles)
        {
            const uint32_t left = triangles - done;
            const size_t batch = static_cast<size_t>(m_Config.batchTriangles);
            const size_t room = batch - std::min(m_BatchCount, batch);
            const uint32_t piece = static_cast<uint32_t>(std::min<size_t>(left, room / 2));
            if (m_Lanes.size() > 1 && !m_Workers.empty() && !m_FrameFull && piece >= kParallelTriangles &&
                ShadeInLanes(part, done, piece))
            {
                done += piece;
                continue;
            }
            const uint32_t step = std::min(left, kParallelTriangles);
            ShadeTriangles(part, done, done + step, m_Lanes[0]);
            done += step;
        }
    }
}

void Raster3D::GeometryLane::EmptyCache()
{
    for (CachedVertex& cached : cache)
    {
        cached.index = UINT32_MAX;
    }
}

void Raster3D::ShadeTriangles(const MeshPart& part, uint32_t first, uint32_t end, GeometryLane& lane)
{
    const Mesh3D& mesh = *part.mesh;
    const VertexLayout& layout = mesh.layout;
    const Deki::Mat4& mvp = *part.mvp;
    const Deki::Mat4& normalMatrix = *part.normalMatrix;
    const Deki::Mat4& model = *part.model;
    const Deki::Vector3 ambient = part.ambient;
    const uint16_t materialIndex = part.materialIndex;
    const ShadingModel shading = part.shading;
    const bool cullBack = part.cullBack;
    const bool vertexLit = part.vertexLit;
    CachedVertex* const cache = lane.cache.data();

    const uint32_t endIndex = part.firstIndex + end * 3;
    for (uint32_t i = part.firstIndex + first * 3; i < endIndex; i += 3)
    {
        CachedVertex* corner[3];
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t index = mesh.IndexAt(i + k);
            CachedVertex& cached = cache[index & (kVertexCacheSize - 1)];
            if (cached.index != index)
            {
                const void* vtx = mesh.VertexAt(index);
                cached.index = index;
                cached.lit = false;
                cached.position = ReadVec3(vtx, layout.positionOffset);
                ClipVertex& cv = cached.clip;
                float w = 1.0f;
                const Deki::Vector3 clip = Deki3D::TransformPoint(mvp, cached.position, w);
                cv.x = clip.x;
                cv.y = clip.y;
                cv.z = clip.z;
                cv.w = w;
                if (layout.HasUVs())
                {
                    ReadUV(vtx, layout.uvOffset, cv.u, cv.v);
                }
                else
                {
                    cv.u = cv.v = 0.0f;
                }
                cv.color = layout.HasColors() ? ReadColor(vtx, layout.colorOffset) : 0xFFFFFFFFu;
                cv.lightR = cv.lightG = cv.lightB = 1.0f;
                if (layout.HasNormals())
                {
                    cached.normal = ReadVec3(vtx, layout.normalOffset);
                }
                cached.inFront = cv.z + cv.w >= 0.0f;  // as EmitTriangle's clip decides it
                cached.screen = Project(cv);
            }
            corner[k] = &cached;
        }
        const ClipVertex& c0 = corner[0]->clip;
        const ClipVertex& c1 = corner[1]->clip;
        const ClipVertex& c2 = corner[2]->clip;

        // Wholly outside one side of the view: nothing to light or bin.
        if ((c0.x < -c0.w && c1.x < -c1.w && c2.x < -c2.w) || (c0.x > c0.w && c1.x > c1.w && c2.x > c2.w) ||
            (c0.y < -c0.w && c1.y < -c1.w && c2.y < -c2.w) || (c0.y > c0.w && c1.y > c1.w && c2.y > c2.w) ||
            (c0.z > c0.w && c1.z > c1.w && c2.z > c2.w) || (c0.z < -c0.w && c1.z < -c1.w && c2.z < -c2.w))
        {
            continue;
        }
        // Facing away, decided from the projected corners when all three
        // are in front of the eye, as BinFan decides it, but before the
        // lighting is paid for. One crossing the near plane is left to
        // BinFan, after the clip.
        if (cullBack && c0.w > 0.0f && c1.w > 0.0f && c2.w > 0.0f)
        {
            const ScreenVertex& s0 = corner[0]->screen;
            const ScreenVertex& s1 = corner[1]->screen;
            const ScreenVertex& s2 = corner[2]->screen;
            if ((s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y) >= 0.0f)
            {
                continue;
            }
        }

        // Lighting is computed here, at vertex rate, so the span loop
        // needs no normals or dot products.
        if (vertexLit)
        {
            for (CachedVertex* v : corner)
            {
                if (v->lit)
                {
                    continue;
                }
                Deki::Vector3 n = Deki3D::TransformDirection(normalMatrix, v->normal);
                if (n.LengthSquared() > 0.0f)
                {
                    n.Normalize();
                }
                float w = 1.0f;
                const Deki::Vector3 at = Deki3D::TransformPoint(model, v->position, w);
                const Deki::Vector3 light = LightAt(ambient, m_MeshLights, m_MeshLightCount, at, n);
                v->clip.lightR = light.x;
                v->clip.lightG = light.y;
                v->clip.lightB = light.z;
                v->lit = true;
            }
            EmitCorners(corner, c0, c1, c2, materialIndex, lane);
        }
        else if (shading == ShadingModel::Flat)
        {
            const Deki::Vector3& p0 = corner[0]->position;
            const Deki::Vector3& p1 = corner[1]->position;
            const Deki::Vector3& p2 = corner[2]->position;
            Deki::Vector3 faceNormal = (p1 - p0).Cross(p2 - p0);
            faceNormal = Deki3D::TransformDirection(normalMatrix, faceNormal);
            if (faceNormal.LengthSquared() > 0.0f)
            {
                faceNormal.Normalize();
            }
            // At the face's middle, with its normal.
            float w = 1.0f;
            const Deki::Vector3 middle = (p0 + p1 + p2) * (1.0f / 3.0f);
            const Deki::Vector3 at = Deki3D::TransformPoint(model, middle, w);
            const Deki::Vector3 light = LightAt(ambient, m_MeshLights, m_MeshLightCount, at, faceNormal);
            ClipVertex cv[3] = { c0, c1, c2 };
            for (ClipVertex& v : cv)
            {
                v.lightR = light.x;
                v.lightG = light.y;
                v.lightB = light.z;
            }
            EmitCorners(corner, cv[0], cv[1], cv[2], materialIndex, lane);
        }
        else
        {
            EmitCorners(corner, c0, c1, c2, materialIndex, lane);
        }
    }
}

void Raster3D::PickLights(const Mesh3D& mesh, const Deki::Mat4& model)
{
    // Every light, if there are few enough; otherwise the ones that reach the
    // mesh most, at its middle, allowing for its size.
    m_MeshLightCount = 0;
    if (m_Config.lightCount <= kLightsPerMesh)
    {
        for (int i = 0; i < m_Config.lightCount; ++i)
        {
            m_MeshLights[m_MeshLightCount++] = m_Config.lights[i];
        }
        return;
    }
    float w = 1.0f;
    const Deki::Vector3 middle = Deki3D::TransformPoint(model, (mesh.boundsMin + mesh.boundsMax) * 0.5f, w);
    const float size = (mesh.boundsMax - mesh.boundsMin).Length() * 0.5f;
    float score[RasterConfig::kMaxLights];
    int order[RasterConfig::kMaxLights];
    for (int i = 0; i < m_Config.lightCount; ++i)
    {
        const Light3D& l = m_Config.lights[i];
        const float strength = l.color.x + l.color.y + l.color.z;
        float reach = 1.0f;
        if (l.kind != Light3D::Kind::Directional)
        {
            const float d = std::max((l.position - middle).Length() - size, 0.0f);
            reach = std::max(1.0f - (d * d) / (l.range * l.range), 0.0f);
        }
        score[i] = strength * reach;
        order[i] = i;
    }
    std::sort(order, order + m_Config.lightCount, [&](int a, int b) { return score[a] > score[b]; });
    for (int i = 0; i < kLightsPerMesh && score[order[i]] > 0.0f; ++i)
    {
        m_MeshLights[m_MeshLightCount++] = m_Config.lights[order[i]];
    }
}

void Raster3D::EmitCorners(const CachedVertex* const corner[3], const ClipVertex& a, const ClipVertex& b,
                           const ClipVertex& c, uint16_t materialIndex, GeometryLane& lane)
{
    if (!corner[0]->inFront || !corner[1]->inFront || !corner[2]->inFront)
    {
        EmitTriangle(a, b, c, materialIndex, lane);
        return;
    }
    // What the clip would pass through unchanged, projected once per vertex.
    const ClipVertex* clip[3] = { &a, &b, &c };
    ScreenVertex sv[3];
    for (int k = 0; k < 3; ++k)
    {
        sv[k] = corner[k]->screen;
        sv[k].color = clip[k]->color;
        sv[k].lightR = clip[k]->lightR;
        sv[k].lightG = clip[k]->lightG;
        sv[k].lightB = clip[k]->lightB;
    }
    BinFan(sv, 3, materialIndex, lane);
}

void Raster3D::EmitTriangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c, uint16_t materialIndex,
                            GeometryLane& lane)
{
    // Near-clip only. Clamping the screen bounding box handles the other
    // four planes, which is cheaper and just as correct for a rasteriser that
    // walks pixels rather than edges.
    const ClipVertex in[3] = { a, b, c };
    ClipVertex out[4];
    int outCount = 0;

    auto dist = [](const ClipVertex& v) { return v.z + v.w; };  // near plane: z + w = 0

    for (int i = 0; i < 3; ++i)
    {
        const ClipVertex& cur = in[i];
        const ClipVertex& next = in[(i + 1) % 3];
        const float dCur = dist(cur);
        const float dNext = dist(next);

        if (dCur >= 0.0f)
        {
            out[outCount++] = cur;
        }

        if ((dCur >= 0.0f) != (dNext >= 0.0f))
        {
            const float t = dCur / (dCur - dNext);
            ClipVertex v;
            v.x = cur.x + (next.x - cur.x) * t;
            v.y = cur.y + (next.y - cur.y) * t;
            v.z = cur.z + (next.z - cur.z) * t;
            v.w = cur.w + (next.w - cur.w) * t;
            v.u = cur.u + (next.u - cur.u) * t;
            v.v = cur.v + (next.v - cur.v) * t;
            v.lightR = cur.lightR + (next.lightR - cur.lightR) * t;
            v.lightG = cur.lightG + (next.lightG - cur.lightG) * t;
            v.lightB = cur.lightB + (next.lightB - cur.lightB) * t;
            v.color = cur.color;  // vertex colour is not interpolated across the cut
            out[outCount++] = v;
            if (outCount == 4)
            {
                break;
            }
        }
    }

    if (outCount >= 3)
    {
        ProjectAndBin(out, outCount, materialIndex, lane);
    }
}

Raster3D::ScreenVertex Raster3D::Project(const ClipVertex& c) const
{
    const float halfW = static_cast<float>(m_Width) * 0.5f;
    const float halfH = static_cast<float>(m_Height) * 0.5f;
    ScreenVertex sv;
    const float invW = (c.w != 0.0f) ? 1.0f / c.w : 0.0f;
    sv.invW = invW;
    sv.x = (c.x * invW + 1.0f) * halfW;
    sv.y = (1.0f - c.y * invW) * halfH;  // clip space is Y up, the framebuffer is Y down
    sv.z = c.z * invW;
    sv.uOverW = c.u * invW;
    sv.vOverW = c.v * invW;
    sv.color = c.color;
    sv.lightR = c.lightR;
    sv.lightG = c.lightG;
    sv.lightB = c.lightB;

    if (m_Config.vertexSnap)
    {
        sv.x = std::floor(sv.x + 0.5f);
        sv.y = std::floor(sv.y + 0.5f);
    }
    return sv;
}

void Raster3D::ProjectAndBin(const ClipVertex* poly, int count, uint16_t materialIndex, GeometryLane& lane)
{
    ScreenVertex sv[4];
    for (int i = 0; i < count; ++i)
    {
        sv[i] = Project(poly[i]);
    }
    BinFan(sv, count, materialIndex, lane);
}

void Raster3D::BinFan(ScreenVertex* sv, int count, uint16_t materialIndex, GeometryLane& lane)
{
    // Fan the clipped polygon back into triangles.
    for (int i = 1; i + 1 < count; ++i)
    {
        RasterTri tri;
        const ScreenVertex* corners[3] = { &sv[0], &sv[i], &sv[i + 1] };
        for (int k = 0; k < 3; ++k)
        {
            const ScreenVertex& c = *corners[k];
            tri.v[k] = TriVertex{ c.x, c.y, c.invW, c.z, c.uOverW, c.vOverW };
        }
        tri.color = sv[0].color;
        tri.lightR = sv[0].lightR;
        tri.lightG = sv[0].lightG;
        tri.lightB = sv[0].lightB;
        tri.materialIndex = materialIndex;

        const float area2 = (tri.v[1].x - tri.v[0].x) * (tri.v[2].y - tri.v[0].y) -
                            (tri.v[2].x - tri.v[0].x) * (tri.v[1].y - tri.v[0].y);

        // Front faces are counter-clockwise in world space, which is negative
        // signed area once Y points down.
        const Material3D& material = m_Materials[materialIndex];
        if (m_Config.backfaceCull && !material.doubleSided && area2 >= 0.0f)
        {
            continue;
        }
        if (area2 == 0.0f)
        {
            continue;
        }

        // The filler's inside test is "all three edge functions >= 0", which
        // needs positive area. Front faces are negative, so they get flipped;
        // a double-sided back face is already positive.
        if (area2 < 0.0f)
        {
            std::swap(tri.v[1], tri.v[2]);
        }

        if (!lane.staging)
        {
            ++m_Stats.trianglesClipped;
            Bin(tri);
            continue;
        }
        // Into the lane's reserved room, for ShadeInLanes to bin in order.
        ++lane.trianglesClipped;
        TileRect rect;
        if (TilesOf(tri, rect))
        {
            lane.tris.PushReserved(tri);
            lane.rects.PushReserved(rect);
        }
    }
}

bool Raster3D::TilesOf(const RasterTri& tri, TileRect& rect) const
{
    float minXf = tri.v[0].x, maxXf = tri.v[0].x;
    float minYf = tri.v[0].y, maxYf = tri.v[0].y;
    for (int i = 1; i < 3; ++i)
    {
        minXf = std::min(minXf, tri.v[i].x);
        maxXf = std::max(maxXf, tri.v[i].x);
        minYf = std::min(minYf, tri.v[i].y);
        maxYf = std::max(maxYf, tri.v[i].y);
    }

    const int minX = std::max(0, FloorToInt(minXf));
    const int minY = std::max(0, FloorToInt(minYf));
    const int maxX = std::min(static_cast<int>(m_Width) - 1, CeilToInt(maxXf));
    const int maxY = std::min(static_cast<int>(m_Height) - 1, CeilToInt(maxYf));
    if (minX > maxX || minY > maxY)
    {
        return false;
    }
    const int tile = m_Config.tileSize;
    rect.x0 = static_cast<int16_t>(minX / tile);
    rect.x1 = static_cast<int16_t>(maxX / tile);
    rect.y0 = static_cast<int16_t>(minY / tile);
    rect.y1 = static_cast<int16_t>(maxY / tile);
    return true;
}

bool Raster3D::Room(const TileRect& rect, bool withTri)
{
    // Room for the triangle and an entry in each tile it touches, or the
    // batch so far is filled first: a full batch, or memory that will not
    // stretch to one.
    if (m_BatchCount >= static_cast<size_t>(m_Config.batchTriangles))
    {
        return false;
    }
    RegionArray<RasterTri>& tris = m_Lanes[0].tris;
    if (withTri && !tris.Reserve(tris.Size() + 1))
    {
        return false;
    }
    for (int ty = rect.y0; ty <= rect.y1; ++ty)
    {
        for (int tx = rect.x0; tx <= rect.x1; ++tx)
        {
            RegionArray<uint32_t>& bin = m_Bins[static_cast<size_t>(ty) * m_TilesX + tx];
            if (!bin.Reserve(bin.Size() + 1))
            {
                return false;
            }
        }
    }
    return true;
}

void Raster3D::PushEntries(uint32_t entry, const TileRect& rect)
{
    ++m_BatchCount;
    for (int ty = rect.y0; ty <= rect.y1; ++ty)
    {
        for (int tx = rect.x0; tx <= rect.x1; ++tx)
        {
            m_Bins[static_cast<size_t>(ty) * m_TilesX + tx].PushReserved(entry);
            ++m_Stats.trianglesBinned;
        }
    }
}

void Raster3D::Bin(const RasterTri& tri)
{
    TileRect rect;
    if (!TilesOf(tri, rect))
    {
        return;
    }
    if (m_FrameFull || (!Room(rect, true) && (!FlushBatch() || !Room(rect, true))))
    {
        ++m_Stats.trianglesDropped;
        return;
    }
    RegionArray<RasterTri>& tris = m_Lanes[0].tris;
    const size_t index = tris.Size();
    tris.PushReserved(tri);
    PushEntries(Entry(0, index), rect);
}

void Raster3D::BinStaged(int lane, size_t index)
{
    const GeometryLane& from = m_Lanes[static_cast<size_t>(lane)];
    const TileRect rect = from.rects[index - from.stagedFrom];
    if (m_FrameFull || (!Room(rect, false) && (!FlushBatch() || !Room(rect, false))))
    {
        ++m_Stats.trianglesDropped;
        return;
    }
    PushEntries(Entry(lane, index), rect);
}

bool Raster3D::ShadeInLanes(const MeshPart& part, uint32_t first, uint32_t count)
{
    // Every lane's room first, here: the lanes must not allocate (the
    // allocator's bookkeeping is the calling thread's), and a triangle makes
    // at most two.
    const uint32_t lanes = static_cast<uint32_t>(m_Lanes.size());
    const uint32_t block = std::min(kGeometryBlock, (count + lanes - 1) / lanes);
    const uint32_t blocks = (count + block - 1) / block;
    for (uint32_t l = 0; l < lanes; ++l)
    {
        GeometryLane& lane = m_Lanes[l];
        const uint32_t own = blocks > l ? (blocks - l + lanes - 1) / lanes : 0;
        const size_t most = static_cast<size_t>(own) * block * 2;
        lane.rects.Clear();
        if (!lane.tris.Reserve(lane.tris.Size() + most) || !lane.rects.Reserve(most))
        {
            return false;
        }
    }
    if (m_BlockEnds.size() < blocks)
    {
        m_BlockEnds.resize(blocks);
    }
    for (GeometryLane& lane : m_Lanes)
    {
        lane.staging = true;
        lane.stagedFrom = lane.tris.Size();
        lane.trianglesClipped = 0;
    }
    m_ShadePart = &part;
    m_ShadeFirst = first;
    m_ShadeCount = count;
    m_ShadeBlock = block;
    RunOnAll(Job::Shade);

    // Into the tiles in the order one thread would have put them: block by
    // block, each from where its lane's last one ended.
    for (uint32_t b = 0; b < blocks; ++b)
    {
        const uint32_t l = b % lanes;
        const size_t start = b < lanes ? m_Lanes[l].stagedFrom : m_BlockEnds[b - lanes];
        for (size_t index = start; index < m_BlockEnds[b]; ++index)
        {
            BinStaged(static_cast<int>(l), index);
        }
    }
    for (GeometryLane& lane : m_Lanes)
    {
        lane.staging = false;
        m_Stats.trianglesClipped += lane.trianglesClipped;
    }
    return true;
}

void Raster3D::ShadeLane(int index)
{
    GeometryLane& lane = m_Lanes[static_cast<size_t>(index)];
    const uint32_t lanes = static_cast<uint32_t>(m_Lanes.size());
    const uint32_t block = m_ShadeBlock;
    const uint32_t blocks = (m_ShadeCount + block - 1) / block;
    for (uint32_t b = static_cast<uint32_t>(index); b < blocks; b += lanes)
    {
        const uint32_t start = m_ShadeFirst + b * block;
        const uint32_t end = std::min(start + block, m_ShadeFirst + m_ShadeCount);
        ShadeTriangles(*m_ShadePart, start, end, lane);
        m_BlockEnds[b] = static_cast<uint32_t>(lane.tris.Size());
    }
}

bool Raster3D::FlushBatch()
{
    if (m_BatchCount == 0)
    {
        return true;  // nothing to make room from: the memory is not there
    }
    if (!m_FrameDepthActive)
    {
        const size_t cells = static_cast<size_t>(m_Width) * m_Height;
        if (m_FrameDepthCells != cells)
        {
            if (m_FrameDepth)
            {
                Deki::Memory::Free(m_FrameDepth);
            }
            m_FrameDepth = Deki::Memory::AllocateArray<uint16_t>(cells, Deki::Memory::External);
            m_FrameDepthCells = m_FrameDepth ? cells : 0;
        }
        if (!m_FrameDepth)
        {
            m_FrameFull = true;
            if (!m_DropReported)
            {
                m_DropReported = true;
                DEKI_LOG_WARNING("Raster3D: a frame has more triangles than one batch (%d), and there is no memory "
                                 "for the %zu-byte depth buffer that drawing it in batches needs: it draws its first "
                                 "batch and no more",
                                 m_Config.batchTriangles, cells * sizeof(uint16_t));
            }
            return false;
        }
        std::fill(m_FrameDepth, m_FrameDepth + cells, static_cast<uint16_t>(kDepthMax));
        m_FrameDepthActive = true;
    }
    FillAll();
    m_BatchCount = 0;
    for (RegionArray<uint32_t>& bin : m_Bins)
    {
        bin.Clear();
    }
    // Mid-way through binning what the lanes made, the lanes keep it: what
    // is still to bin is there.
    if (!m_Lanes[0].staging)
    {
        for (GeometryLane& lane : m_Lanes)
        {
            lane.tris.Clear();
        }
    }
    return true;
}

size_t Raster3D::WorkingBytes(int32_t width, int32_t height, const RasterConfig& config)
{
    const size_t batch = static_cast<size_t>(std::max(config.batchTriangles, 1));
    const int tile = std::max(config.tileSize, 8);
    const size_t tiles = static_cast<size_t>((width + tile - 1) / tile) * ((height + tile - 1) / tile);
    const int threads = std::max(config.threadCount, 1);
    // A triangle's tile entries: most touch one or two tiles; four each is
    // the bound for one no larger than a tile, and larger ones are few. And
    // its tiles while the threads share the geometry out (ShadeInLanes).
    return batch * sizeof(RasterTri) + batch * 4 * sizeof(uint32_t) + batch * sizeof(TileRect) +
           tiles * sizeof(RegionArray<uint32_t>) +
           static_cast<size_t>(width) * height * sizeof(uint16_t) +
           static_cast<size_t>(threads) * tile * tile * sizeof(uint16_t);
}

// ---------------------------------------------------------------------------
// Fill
// ---------------------------------------------------------------------------

void Raster3D::StartWorkers(int count)
{
    StopWorkers();
    if (count <= 0)
    {
        return;
    }
    m_StopWorkers = false;
    m_WorkerStats.assign(static_cast<size_t>(count), RasterStats{});
    m_WorkerHasWork.assign(static_cast<size_t>(count), 0);
    // Every worker plus the calling thread. Set before any thread exists, so
    // no worker reads a container the main thread is still growing.
    m_FillStride = count + 1;
    m_WorkersBusy = 0;
    m_Workers.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
#if defined(ESP_PLATFORM) && !defined(CONFIG_FREERTOS_UNICORE)
        // On the core the engine is not on, as Lighting2D's worker is: a
        // std::thread there starts wherever the scheduler puts it, which was
        // the engine's own core, so the fill gained nothing from a second.
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        cfg.pin_to_core = (xPortGetCoreID() + 1 + i) % portNUM_PROCESSORS;
        cfg.prio = uxTaskPriorityGet(nullptr);
        cfg.thread_name = "deki_fill";
        // Room for the geometry's clip and lighting as well as the fill.
        cfg.stack_size = std::max<size_t>(cfg.stack_size, 4096);
        esp_pthread_set_cfg(&cfg);
#endif
        m_Workers.emplace_back([this, i] { WorkerLoop(i); });
    }
}

void Raster3D::StopWorkers()
{
    if (m_Workers.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_WorkMutex);
        m_StopWorkers = true;
    }
    m_WorkReady.notify_all();
    for (std::thread& worker : m_Workers)
    {
        worker.join();
    }
    m_Workers.clear();
    m_WorkerStats.clear();
    m_WorkerHasWork.clear();
}

void Raster3D::WorkerLoop(int index)
{
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_WorkMutex);
            m_WorkReady.wait(lock, [this, index]
                             { return m_StopWorkers || m_WorkerHasWork[static_cast<size_t>(index)] != 0; });
            if (m_StopWorkers)
            {
                return;
            }
            m_WorkerHasWork[static_cast<size_t>(index)] = 0;
        }

        // Outside the lock. Worker `index` takes every stride-th tile, or
        // lane index + 1, and touches nothing another worker touches. The
        // stride is fixed before any worker starts; m_Workers.size() may
        // still be growing.
        if (m_Job == Job::Fill)
        {
            FillTileRange(index, m_FillStride, index, m_WorkerStats[index]);
        }
        else
        {
            ShadeLane(index + 1);
        }

        {
            std::lock_guard<std::mutex> lock(m_WorkMutex);
            if (--m_WorkersBusy == 0)
            {
                m_WorkDone.notify_one();
            }
        }
    }
}

void Raster3D::SetBackground(const uint8_t* pixel)
{
    std::memcpy(m_Background, pixel, std::min(m_BytesPerPixel, sizeof(m_Background)));
    m_HasBackground = true;
}

void Raster3D::FillBackground(int originX, int originY, int maxX, int maxY)
{
    // Before the tile's triangles: its lines of the frame come into the cache
    // once, here, and the triangles write into them there.
    const int count = maxX - originX + 1;
    for (int y = originY; y <= maxY; ++y)
    {
        uint8_t* row = m_Buffer + (static_cast<size_t>(y) * m_Width + originX) * m_BytesPerPixel;
        if (m_BytesPerPixel == 2)
        {
            uint16_t pixel;
            std::memcpy(&pixel, m_Background, 2);
            std::fill_n(reinterpret_cast<uint16_t*>(row), count, pixel);
            continue;
        }
        for (int x = 0; x < count; ++x)
        {
            std::memcpy(row + static_cast<size_t>(x) * m_BytesPerPixel, m_Background, m_BytesPerPixel);
        }
    }
}

void Raster3D::EndFrame()
{
    if (!m_Buffer)
    {
        return;
    }
    FillAll();
}

void Raster3D::FillAll()
{
    ++m_Stats.batches;
    const auto started = std::chrono::steady_clock::now();
    struct Timer
    {
        RasterStats& stats;
        std::chrono::steady_clock::time_point started;
        ~Timer()
        {
            stats.fillMicros += static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                    .count());
        }
    } timer{ m_Stats, started };
    const int tileCount = m_TilesX * m_TilesY;
    const int threads = static_cast<int>(m_TileDepth.size());
    // The background goes only into the frame's first fill: later batches
    // draw over it.
    struct BackgroundOnce
    {
        bool& has;
        ~BackgroundOnce() { has = false; }
    } backgroundOnce{ m_HasBackground };

    if (threads <= 1 || tileCount <= 1 || m_Workers.empty())
    {
        FillTileRange(0, 1, 0, m_Stats);
        return;
    }

    // Interleaved, not contiguous: geometry clusters, so solid blocks of
    // screen would give one thread the empty tiles and another all the work.
    // Striding spreads a cluster across all threads.
    for (RasterStats& s : m_WorkerStats)
    {
        s.Reset();
    }

    RunOnAll(Job::Fill);

    for (const RasterStats& s : m_WorkerStats)
    {
        m_Stats.pixelsTested += s.pixelsTested;
        m_Stats.pixelsWritten += s.pixelsWritten;
    }
}

void Raster3D::RunOnAll(Job job)
{
    const int workerCount = static_cast<int>(m_Workers.size());
    {
        std::lock_guard<std::mutex> lock(m_WorkMutex);
        m_Job = job;
        m_WorkersBusy = workerCount;
        for (char& hasWork : m_WorkerHasWork)
        {
            hasWork = 1;
        }
    }
    m_WorkReady.notify_all();

    // The calling thread takes a share instead of idling: the last tiles, or
    // lane 0.
    if (job == Job::Fill)
    {
        FillTileRange(workerCount, m_FillStride, workerCount, m_Stats);
    }
    else
    {
        ShadeLane(0);
    }

    std::unique_lock<std::mutex> lock(m_WorkMutex);
    m_WorkDone.wait(lock, [this] { return m_WorkersBusy == 0; });
}

Raster3D::~Raster3D()
{
    StopWorkers();
    if (m_FrameDepth)
    {
        Deki::Memory::Free(m_FrameDepth);
    }
}

void Raster3D::FillTileRange(int start, int stride, int thread, RasterStats& stats)
{
    const int tileCount = m_TilesX * m_TilesY;
    for (int index = start; index < tileCount; index += stride)
    {
        FillTile(index % m_TilesX, index / m_TilesX, thread, stats);
    }
}

void Raster3D::FillTile(int tileX, int tileY, int thread, RasterStats& stats)
{
    const RegionArray<uint32_t>& bin = m_Bins[static_cast<size_t>(tileY) * m_TilesX + tileX];
    if (bin.Empty() && !m_HasBackground)
    {
        return;
    }

    const int tile = m_Config.tileSize;
    const int originX = tileX * tile;
    const int originY = tileY * tile;
    const int maxX = std::min(originX + tile, static_cast<int>(m_Width)) - 1;
    const int maxY = std::min(originY + tile, static_cast<int>(m_Height)) - 1;

    // In batches, the frame's depth, which earlier batches wrote. Otherwise
    // this thread's scratch buffer, reset and reused for every tile it takes.
    uint16_t* depth = m_TileDepth[static_cast<size_t>(thread)].data();
    int depthStride = tile;
    if (m_FrameDepthActive)
    {
        depth = m_FrameDepth + static_cast<size_t>(originY) * m_Width + originX;
        depthStride = m_Width;
    }
    else
    {
        std::fill(depth, depth + static_cast<size_t>(tile) * tile, static_cast<uint16_t>(kDepthMax));
    }

    if (m_HasBackground)
    {
        FillBackground(originX, originY, maxX, maxY);
    }
    for (uint32_t entry : bin)
    {
        FillTriangleInTile(TriAt(entry), originX, originY, maxX, maxY, depth, depthStride, originX, originY, stats);
    }
}

void Raster3D::FillTriangleInTile(const RasterTri& tri, int minX, int minY, int maxX, int maxY, uint16_t* depth,
                                  int depthStride, int tileOriginX, int tileOriginY, RasterStats& stats)
{
    const TriVertex& v0 = tri.v[0];
    const TriVertex& v1 = tri.v[1];
    const TriVertex& v2 = tri.v[2];

    // Clamp the walk to where this triangle actually is inside this tile.
    const float loX = std::min(v0.x, std::min(v1.x, v2.x));
    const float hiX = std::max(v0.x, std::max(v1.x, v2.x));
    const float loY = std::min(v0.y, std::min(v1.y, v2.y));
    const float hiY = std::max(v0.y, std::max(v1.y, v2.y));
    int bMinX = std::max(minX, FloorToInt(loX));
    int bMaxX = std::min(maxX, CeilToInt(hiX));
    int bMinY = std::max(minY, FloorToInt(loY));
    int bMaxY = std::min(maxY, CeilToInt(hiY));
    if (bMinX > bMaxX || bMinY > bMaxY)
    {
        return;
    }

    // A floor rather than lrint: lrint honours the current rounding mode, so
    // it stays a libm call.
    auto fixed = [](float f) { return static_cast<int32_t>(FloorToInt(f * kSubScale + 0.5f)); };
    const int32_t x0 = fixed(v0.x), y0 = fixed(v0.y);
    const int32_t x1 = fixed(v1.x), y1 = fixed(v1.y);
    const int32_t x2 = fixed(v2.x), y2 = fixed(v2.y);

    // Edge functions, evaluated at the centre of the first pixel and stepped
    // by adds from there. One pixel is kSubScale in 28.4.
    const int32_t a01 = y0 - y1, b01 = x1 - x0;
    const int32_t a12 = y1 - y2, b12 = x2 - x1;
    const int32_t a20 = y2 - y0, b20 = x0 - x2;

    const int32_t px = bMinX * kSubScale + kSubScale / 2;
    const int32_t py = bMinY * kSubScale + kSubScale / 2;

    // A triangle within 512 pixels of the origin, as one on screen is, has
    // edge values well inside 32 bits (corner differences under 2^14 in
    // 28.4, products under 2^28), so its setup and walk need no 64-bit
    // arithmetic, which a 32-bit core does in several instructions each. One
    // reaching further off-screen takes the 64-bit setup and the check below.
    constexpr int32_t kNear = 1 << 13;
    auto near = [](int32_t v) { return v > -kNear && v < kNear; };
    const bool small = near(x0) && near(y0) && near(x1) && near(y1) && near(x2) && near(y2);
    auto edge = [small](int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) -> int64_t
    {
        if (small)
        {
            return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        }
        return static_cast<int64_t>(bx - ax) * (py - ay) - static_cast<int64_t>(by - ay) * (px - ax);
    };

    int64_t rowE0 = edge(x1, y1, x2, y2, px, py);  // weight of v0
    int64_t rowE1 = edge(x2, y2, x0, y0, px, py);  // weight of v1
    int64_t rowE2 = edge(x0, y0, x1, y1, px, py);  // weight of v2

    // dE/dpx is A, dE/dpy is B, both scaled by one pixel in 28.4. E0 is the
    // edge opposite v0, so it steps with the v1->v2 edge, and so on round.
    const int64_t stepX0 = static_cast<int64_t>(a12) * kSubScale;
    const int64_t stepY0 = static_cast<int64_t>(b12) * kSubScale;
    const int64_t stepX1 = static_cast<int64_t>(a20) * kSubScale;
    const int64_t stepY1 = static_cast<int64_t>(b20) * kSubScale;
    const int64_t stepX2 = static_cast<int64_t>(a01) * kSubScale;
    const int64_t stepY2 = static_cast<int64_t>(b01) * kSubScale;

    const float area2 = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (area2 == 0.0f)
    {
        return;
    }
    const float invArea = Reciprocal(area2);

    const Gradient gInvW = MakeGradient(v0.invW, v1.invW, v2.invW, v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea);
    const Gradient gZ = MakeGradient(v0.z, v1.z, v2.z, v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea);
    // Perspective-correct interpolates u/w and v/w, which are linear in screen
    // space, and divides back at the span endpoints. Affine interpolates u and
    // v directly, which are not: that error is the PlayStation-era warp, and
    // it grows with the triangle's screen size.
    const bool affine = !m_Config.perspectiveCorrect;
    auto vertexU = [](const TriVertex& v) { return v.invW != 0.0f ? v.uOverW / v.invW : 0.0f; };
    auto vertexV = [](const TriVertex& v) { return v.invW != 0.0f ? v.vOverW / v.invW : 0.0f; };

    const Gradient gU =
        affine ? MakeGradient(vertexU(v0), vertexU(v1), vertexU(v2), v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea)
               : MakeGradient(v0.uOverW, v1.uOverW, v2.uOverW, v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea);
    const Gradient gV =
        affine ? MakeGradient(vertexV(v0), vertexV(v1), vertexV(v2), v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea)
               : MakeGradient(v0.vOverW, v1.vOverW, v2.vOverW, v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea);

    const Material3D& mat = m_Materials[tri.materialIndex];
    const Texture3D* tex = mat.texture;
    const bool textured = tex && tex->Valid();
    const TexKind kind = KindOfTexture(tex);
    const int uMask = textured ? (tex->width - 1) : 0;
    const int vMask = textured ? (tex->height - 1) : 0;
    const int wShift = textured ? tex->widthShift : 0;

    // Flat colour when there is no texture: the first vertex's colour times
    // the material tint. Vertex colours are not interpolated.
    const uint32_t tint = mat.tint;
    const uint32_t flatRgba = ((tri.color & 0xFF) * (tint & 0xFF) / 255) |
                              ((((tri.color >> 8) & 0xFF) * ((tint >> 8) & 0xFF) / 255) << 8) |
                              ((((tri.color >> 16) & 0xFF) * ((tint >> 16) & 0xFF) / 255) << 16);
    const uint16_t flatColor = To565(flatRgba);

    const bool lit = mat.shading != ShadingModel::Unlit;
    // The light of the triangle's first vertex, per channel: the same for every span.
    auto toShade = [](float l) { return static_cast<uint32_t>(std::min(64.0f, std::max(0.0f, l * 32.0f))); };
    const uint32_t shadeR = lit ? toShade(tri.lightR) : 32u;
    const uint32_t shadeG = lit ? toShade(tri.lightG) : 32u;
    const uint32_t shadeB = lit ? toShade(tri.lightB) : 32u;
    const int subdiv = m_Config.perspectiveCorrect ? m_Config.spanSubdivision : (bMaxX - bMinX + 1);

    // What every span of this triangle shares, and the loop that fills them,
    // set once: a span then sets only where it is and what it steps.
    SpanArgs span;
    span.tex = tex;
    span.alphaTest = mat.alphaTest;
    span.lit = lit;
    span.shadeR = shadeR;
    span.shadeG = shadeG;
    span.shadeB = shadeB;
    span.grey = shadeR == shadeG && shadeG == shadeB && shadeR <= 32u;
    span.flat565 = flatColor;
    span.flatR = static_cast<uint8_t>(flatRgba & 0xFF);
    span.flatG = static_cast<uint8_t>((flatRgba >> 8) & 0xFF);
    span.flatB = static_cast<uint8_t>((flatRgba >> 16) & 0xFF);
    span.uMask = uMask;
    span.vMask = vMask;
    span.wShift = wShift;
    span.depthTest = m_Config.depthTest;
    span.depthWrite = m_Config.depthWrite;
    const SpanFn fill = ChooseSpan(kind, m_Format, span);
    // Texels in 16.16 per unit of u and v. The width times 2^16 is exact, so
    // this is the product it was when the span multiplied by each in turn.
    const float uScale = textured ? static_cast<float>(tex->width) * (1 << kUvFrac) : 0.0f;
    const float vScale = textured ? static_cast<float>(tex->height) * (1 << kUvFrac) : 0.0f;
    const float* reciprocal = Reciprocals();

    // What a span works out, per unit of x from a row's base: u/w and v/w
    // already in texels (16.16), z already in depth units, so a span scales
    // nothing; and each row's value at x = 0 worked out once, so a span
    // keeps only the base and the step of each, which the FPU's registers
    // hold, where the whole gradients of all four did not and were read back
    // from the stack every span.
    auto scaled = [](Gradient g, float k, float offset)
    {
        g.at0 = g.at0 * k + offset;
        g.dx *= k;
        g.dy *= k;
        return g;
    };
    constexpr float kDepthScale = static_cast<float>(kDepthMax) * 0.5f * (1 << kDepthFrac);
    constexpr float kDepthTop = static_cast<float>(kDepthMax) * (1 << kDepthFrac);
    const Gradient sU = scaled(gU, uScale, 0.0f);
    const Gradient sV = scaled(gV, vScale, 0.0f);
    const Gradient sZ = scaled(gZ, kDepthScale, kDepthScale);
    // Depth is clamped to its range only where a corner lies outside it: the
    // pixels between lie between the corners.
    auto inRange = [&](float ndcZ)
    {
        const float d = ndcZ * kDepthScale + kDepthScale;
        return d >= 0.0f && d <= kDepthTop;
    };
    const bool clampZ = !inRange(v0.z) || !inRange(v1.z) || !inRange(v2.z);
    auto toDepth = [clampZ, kDepthTop](float d) -> int32_t
    {
        if (clampZ)
        {
            d = std::min(std::max(d, 0.0f), kDepthTop);
        }
        return static_cast<int32_t>(d);
    };

    // The edge walk adds three edge values per pixel. A 32-bit core takes
    // several instructions for a 64-bit add, so it walks in 32 bits wherever
    // the values over this tile's part of the triangle fit, which is any
    // triangle near the screen's size; only one reaching far off-screen needs
    // 64. The values are linear, so the corners of the walk bound them (the
    // walk steps once past its last pixel and row).
    const int64_t spanX = bMaxX - bMinX + 1;
    const int64_t spanY = bMaxY - bMinY + 1;
    auto fits32 = [&](int64_t start, int64_t stepX, int64_t stepY)
    {
        for (int64_t cx : { int64_t(0), spanX })
        {
            for (int64_t cy : { int64_t(0), spanY })
            {
                const int64_t value = start + cx * stepX + cy * stepY;
                if (value <= INT32_MIN || value > INT32_MAX)
                {
                    return false;
                }
            }
        }
        return stepX > INT32_MIN && stepX <= INT32_MAX && stepY > INT32_MIN && stepY <= INT32_MAX;
    };
    const bool walk32 =
        small || (fits32(rowE0, stepX0, stepY0) && fits32(rowE1, stepX1, stepY1) && fits32(rowE2, stepX2, stepY2));

    // The corners by height. A row above the middle one is bounded by the
    // two edges from the top corner, and a row from it down by the two to the
    // bottom corner: those two meet at a corner of the triangle, and between
    // them on that side of the middle lies only the triangle, so the third
    // edge cannot narrow the run. Two bounds a row, not three, and the same
    // pixels.
    const int32_t cornerY[3] = { y0, y1, y2 };
    int top = 0;
    int bottom = 0;
    for (int k = 1; k < 3; ++k)
    {
        top = cornerY[k] < cornerY[top] ? k : top;
        bottom = cornerY[k] >= cornerY[bottom] ? k : bottom;
    }
    const bool level = top == bottom || cornerY[top] == cornerY[bottom];
    const int middle = level ? 0 : 3 - top - bottom;
    const int32_t middleY = cornerY[middle];

    uint32_t tested = 0;
    uint32_t written = 0;
    auto walk = [&](auto zero)
    {
        using Edge = decltype(zero);
        const Edge rowEdgeStart[3] = { static_cast<Edge>(rowE0), static_cast<Edge>(rowE1), static_cast<Edge>(rowE2) };
        const Edge stepXOf[3] = { static_cast<Edge>(stepX0), static_cast<Edge>(stepX1), static_cast<Edge>(stepX2) };
        const Edge stepYOf[3] = { static_cast<Edge>(stepY0), static_cast<Edge>(stepY1), static_cast<Edge>(stepY2) };
        // Edge k is the one opposite corner k (rowE0 is v0's weight): the long
        // edge is opposite the middle corner, the top edge opposite the bottom
        // one and the bottom edge opposite the top one. Level in 28.4 (a
        // sliver), all three bound every row, as for any triangle.
        const int a = level ? 0 : middle;
        const int b = level ? 1 : bottom;
        const int c = level ? 2 : top;
        Edge rowA = rowEdgeStart[a], rowB = rowEdgeStart[b], rowC = rowEdgeStart[c];
        const Edge stepXA = stepXOf[a], stepXB = stepXOf[b], stepXC = stepXOf[c];
        const Edge stepYA = stepYOf[a], stepYB = stepYOf[b], stepYC = stepYOf[c];
        int32_t rowY = py;  // the row centre, in 28.4
        for (int row = bMinY; row <= bMaxY; ++row)
        {
            // This row's covered run, from the edges rather than by testing
            // each pixel of the box: each edge is linear in x, so where it is
            // >= 0 starts or ends at one division, and a triangle is convex,
            // so the run is the one stretch where they all are. Exactly the
            // pixels the test finds; a model of thin triangles has boxes that
            // are mostly empty.
            int spanStart = bMinX;
            int spanEnd = bMaxX;
            const Edge last = static_cast<Edge>(bMaxX - bMinX);
            auto bound = [&](Edge start, Edge step)
            {
                if (step > 0)
                {
                    // Rises with x: from the first pixel where it reaches 0.
                    if (start < 0)
                    {
                        Edge k = (-start) / step;
                        if ((-start) % step != 0)
                        {
                            ++k;
                        }
                        spanStart = k > last ? bMaxX + 1 : std::max(spanStart, bMinX + static_cast<int>(k));
                    }
                }
                else if (step < 0)
                {
                    // Falls with x: up to the last pixel still at or above 0.
                    if (start < 0)
                    {
                        spanEnd = bMinX - 1;
                    }
                    else
                    {
                        const Edge k = start / (-step);
                        if (k < last)
                        {
                            spanEnd = std::min(spanEnd, bMinX + static_cast<int>(k));
                        }
                    }
                }
                else if (start < 0)
                {
                    spanEnd = bMinX - 1;  // level with the row and outside it
                }
            };
            bound(rowA, stepXA);
            if (level || rowY < middleY)
            {
                bound(rowB, stepXB);
            }
            if (level || rowY >= middleY)
            {
                bound(rowC, stepXC);
            }
            if (spanStart <= spanEnd)
            {
                // The row's values at x = 0, from which its spans step.
                const float fy = static_cast<float>(row) + 0.5f - v0.y;
                const float fx = 0.5f - v0.x;
                const float baseW = gInvW.at0 + gInvW.dy * fy + gInvW.dx * fx;
                const float baseU = sU.at0 + sU.dy * fy + sU.dx * fx;
                const float baseV = sV.at0 + sV.dy * fy + sV.dx * fx;
                const float baseZ = sZ.at0 + sZ.dy * fy + sZ.dx * fx;
                uint8_t* const fbRow = m_Buffer + static_cast<size_t>(row) * m_Width * m_BytesPerPixel;
                uint16_t* const depthRow =
                    depth + static_cast<ptrdiff_t>(row - tileOriginY) * depthStride - tileOriginX;
                int runX = spanStart;
                while (runX <= spanEnd)
                {
                    const int chunkEnd = std::min(runX + subdiv - 1, spanEnd);
                    const float xs = static_cast<float>(runX);
                    const float xe = static_cast<float>(chunkEnd);
                    const int chunkLen = chunkEnd - runX + 1;
                    const float inv = chunkLen <= 1                  ? 0.0f
                                      : chunkLen - 1 <= kReciprocals ? reciprocal[chunkLen - 1]
                                                                     : 1.0f / static_cast<float>(chunkLen - 1);

                    // The only divide in the whole loop: one per span, for
                    // the w at both its ends, and none on the affine path
                    // (Reciprocal: inline where a divide is a library call).
                    int32_t u = 0, v = 0, du = 0, dv = 0;
                    if (textured)
                    {
                        float uS = baseU + sU.dx * xs, uE = baseU + sU.dx * xe;
                        float vS = baseV + sV.dx * xs, vE = baseV + sV.dx * xe;
                        if (!affine)
                        {
                            const float invW0 = baseW + gInvW.dx * xs;
                            const float invW1 = baseW + gInvW.dx * xe;
                            const float both = invW0 * invW1;
                            const float r = (both != 0.0f) ? Reciprocal(both) : 0.0f;
                            const float w0 = invW1 * r;
                            const float w1 = invW0 * r;
                            uS *= w0;
                            vS *= w0;
                            uE *= w1;
                            vE *= w1;
                        }
                        u = static_cast<int32_t>(uS);
                        v = static_cast<int32_t>(vS);
                        du = (chunkLen > 1) ? static_cast<int32_t>((static_cast<int32_t>(uE) - u) * inv) : 0;
                        dv = (chunkLen > 1) ? static_cast<int32_t>((static_cast<int32_t>(vE) - v) * inv) : 0;
                    }

                    // Depth, also stepped with adds.
                    const int32_t z = toDepth(baseZ + sZ.dx * xs);
                    const int32_t zE = toDepth(baseZ + sZ.dx * xe);
                    const int32_t dz = (chunkLen > 1) ? static_cast<int32_t>((zE - z) * inv) : 0;

                    // Row bases, so the span indexes by x alone: a per-pixel
                    // multiply for the tile offset costs more than the depth
                    // test it feeds.
                    span.fbRow = fbRow;
                    span.depthRow = depthRow;
                    span.x0 = runX;
                    span.x1 = chunkEnd;
                    span.u = u;
                    span.v = v;
                    span.z = z;
                    span.du = du;
                    span.dv = dv;
                    span.dz = dz;

                    // ---- the inner loop: integer only, one instance per format pair ----
                    written += fill(span);
                    tested += static_cast<uint32_t>(chunkLen);
                    runX = chunkEnd + 1;
                }
            }

            rowA += stepYA;
            rowB += stepYB;
            rowC += stepYC;
            rowY += kSubScale;
        }
    };
    if (walk32)
    {
        walk(int32_t{});
    }
    else
    {
        walk(int64_t{});
    }
    stats.pixelsTested += tested;
    stats.pixelsWritten += written;
}

}  // namespace Deki3D
