#include "Raster3D.h"
#include "Math3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace Deki3D
{
namespace
{

// Screen coordinates are 28.4: four fractional bits give the edge functions
// sub-pixel precision without the products leaving 64 bits.
constexpr int kSubBits = 4;
constexpr int kSubScale = 1 << kSubBits;

// Depth is 24.8 over a 16-bit depth range. 16 fractional bits would put the
// accumulator past INT32_MAX at the far plane; eight is far more than the
// depth buffer can resolve anyway.
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

/// Scale an RGB565 pixel by shade in 0..32, two multiplies rather than three.
inline uint16_t Shade565(uint16_t c, uint32_t shade)
{
    const uint32_t rb = ((static_cast<uint32_t>(c) & 0xF81Fu) * shade >> 5) & 0xF81Fu;
    const uint32_t g = ((static_cast<uint32_t>(c) & 0x07E0u) * shade >> 5) & 0x07E0u;
    return static_cast<uint16_t>(rb | g);
}

/// Plane gradients of a screen-linear attribute across a triangle. invW, z,
/// u/w and v/w are all linear in screen space, which is the whole reason the
/// span loop can step them with adds.
struct Gradient
{
    float dx = 0.0f;
    float dy = 0.0f;
    float at0 = 0.0f;
};

inline Gradient MakeGradient(float f0, float f1, float f2,
                             float x0, float y0, float x1, float y1, float x2, float y2,
                             float invArea)
{
    Gradient g;
    g.at0 = f0;
    g.dx = ((f1 - f0) * (y2 - y0) - (f2 - f0) * (y1 - y0)) * invArea;
    g.dy = ((f2 - f0) * (x1 - x0) - (f1 - f0) * (x2 - x0)) * invArea;
    return g;
}

inline float Eval(const Gradient& g, float dxFromV0, float dyFromV0)
{
    return g.at0 + g.dx * dxFromV0 + g.dy * dyFromV0;
}

/// Is the mesh's bounding box wholly outside the view?
///
/// The eight corners go to clip space and each of the six planes is asked
/// whether every corner failed it. That test is conservative in the right
/// direction: a box straddling two planes without touching the frustum is
/// not culled, but nothing visible is ever culled, which is the property
/// that matters. It costs eight matrix transforms against the thousands the
/// mesh would otherwise cost, so it pays for itself on the second triangle.
bool BoundsOutsideFrustum(const Mesh3D& mesh, const Deki::Mat4& mvp)
{
    // A mesh whose bounds were never filled in has min == max == 0, and
    // culling on that would hide it whenever the origin left the view.
    if (mesh.boundsMin.x > mesh.boundsMax.x || mesh.boundsMin.y > mesh.boundsMax.y ||
        mesh.boundsMin.z > mesh.boundsMax.z)
        return false;
    if (mesh.boundsMin.x == mesh.boundsMax.x && mesh.boundsMin.y == mesh.boundsMax.y &&
        mesh.boundsMin.z == mesh.boundsMax.z)
        return false;

    int outside[6] = { 0, 0, 0, 0, 0, 0 };
    for (int corner = 0; corner < 8; ++corner)
    {
        const Deki::Vector3 point((corner & 1) ? mesh.boundsMax.x : mesh.boundsMin.x,
                                  (corner & 2) ? mesh.boundsMax.y : mesh.boundsMin.y,
                                  (corner & 4) ? mesh.boundsMax.z : mesh.boundsMin.z);
        float w = 1.0f;
        const Deki::Vector3 clip = Deki3D::TransformPoint(mvp, point, w);
        if (clip.x < -w) ++outside[0];
        if (clip.x > w) ++outside[1];
        if (clip.y < -w) ++outside[2];
        if (clip.y > w) ++outside[3];
        if (clip.z < -w) ++outside[4];
        if (clip.z > w) ++outside[5];
    }
    for (int plane = 0; plane < 6; ++plane)
        if (outside[plane] == 8)
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame setup
// ---------------------------------------------------------------------------

void Raster3D::BeginFrame(uint8_t* buffer, int32_t width, int32_t height,
                          Deki::ColorFormat format, const RasterConfig& config)
{
    m_Buffer = buffer;
    m_Width = width;
    m_Height = height;
    m_Format = format;
    m_Config = config;
    if (m_Config.tileSize < 8)
        m_Config.tileSize = 8;
    if (m_Config.spanSubdivision < 1)
        m_Config.spanSubdivision = 1;

    m_LightDir = m_Config.lightDirection;
    if (m_LightDir.LengthSquared() > 0.0f)
        m_LightDir.Normalize();

    const int tile = m_Config.tileSize;
    m_TilesX = (width + tile - 1) / tile;
    m_TilesY = (height + tile - 1) / tile;

    // Keep the capacity the last frame grew into. Reassigning these would
    // free and reallocate one vector per tile every frame, which costs more
    // than the rasterising does on light scenes.
    m_Tris.clear();
    m_Materials.clear();
    const size_t tileCount = static_cast<size_t>(m_TilesX) * m_TilesY;
    if (m_Bins.size() != tileCount)
        m_Bins.assign(tileCount, {});
    else
        for (std::vector<uint32_t>& bin : m_Bins)
            bin.clear();

    // One scratch depth buffer per fill thread, sized once and kept. The
    // calling thread is one of them, so `threads - 1` workers are needed.
    const int threads = m_Config.threadCount < 1 ? 1 : m_Config.threadCount;
    const size_t depthCells = static_cast<size_t>(tile) * tile;
    if (static_cast<int>(m_TileDepth.size()) != threads)
        m_TileDepth.assign(static_cast<size_t>(threads), {});
    for (std::vector<uint16_t>& buffer : m_TileDepth)
        if (buffer.size() != depthCells)
            buffer.resize(depthCells);

    if (static_cast<int>(m_Workers.size()) != threads - 1)
        StartWorkers(threads - 1);

    m_Stats.Reset();
}

// ---------------------------------------------------------------------------
// Geometry: transform, clip, project, bin
// ---------------------------------------------------------------------------

void Raster3D::DrawMesh(const Mesh3D& mesh, const Material3D* materials, uint16_t materialCount,
                        const Deki::Mat4& mvp, const Deki::Mat4& normalMatrix)
{
    if (!mesh.vertices || !mesh.indices || mesh.layout.positionOffset < 0)
        return;

    // Eight transforms to decide whether thousands are needed at all.
    if (BoundsOutsideFrustum(mesh, mvp))
    {
        ++m_Stats.meshesCulled;
        return;
    }

    const VertexLayout& layout = mesh.layout;

    for (uint16_t s = 0; s < mesh.submeshCount; ++s)
    {
        const Submesh3D& sub = mesh.submeshes[s];
        // Copy the material now: the span loop reads it in EndFrame, after
        // this call has returned and the caller's own object may be gone.
        if (m_Materials.size() >= 0xFFFFu)
            return;
        const uint16_t materialIndex = static_cast<uint16_t>(m_Materials.size());
        m_Materials.push_back((materials && sub.materialIndex < materialCount)
                                  ? materials[sub.materialIndex]
                                  : Material3D{});
        // By value: m_Materials can reallocate on the next submesh, and a
        // pointer into it would not survive that.
        const ShadingModel shading = m_Materials.back().shading;

        const uint32_t end = sub.firstIndex + sub.indexCount;
        for (uint32_t i = sub.firstIndex; i + 2 < end; i += 3)
        {
            ++m_Stats.trianglesIn;

            ClipVertex cv[3];
            Deki::Vector3 objPos[3];
            Deki::Vector3 objNormal[3];

            for (int k = 0; k < 3; ++k)
            {
                const void* vtx = mesh.VertexAt(mesh.indices[i + k]);
                objPos[k] = ReadVec3(vtx, layout.positionOffset);

                float w = 1.0f;
                const Deki::Vector3 clip = Deki3D::TransformPoint(mvp, objPos[k], w);
                cv[k].x = clip.x;
                cv[k].y = clip.y;
                cv[k].z = clip.z;
                cv[k].w = w;

                if (layout.HasUVs())
                    ReadUV(vtx, layout.uvOffset, cv[k].u, cv[k].v);
                else
                    cv[k].u = cv[k].v = 0.0f;

                cv[k].color = layout.HasColors() ? ReadColor(vtx, layout.colorOffset) : 0xFFFFFFFFu;
                cv[k].light = 1.0f;

                if (layout.HasNormals())
                    objNormal[k] = ReadVec3(vtx, layout.normalOffset);
            }

            // Shading is decided here, at vertex rate, so the span loop never
            // touches a normal or a dot product.
            if (shading == ShadingModel::VertexLit && layout.HasNormals())
            {
                for (int k = 0; k < 3; ++k)
                {
                    Deki::Vector3 n = Deki3D::TransformDirection(normalMatrix, objNormal[k]);
                    if (n.LengthSquared() > 0.0f)
                        n.Normalize();
                    const float d = -n.Dot(m_LightDir);
                    cv[k].light = m_Config.ambient + (d > 0.0f ? d : 0.0f) * (1.0f - m_Config.ambient);
                }
            }
            else if (shading == ShadingModel::Flat)
            {
                Deki::Vector3 faceNormal = (objPos[1] - objPos[0]).Cross(objPos[2] - objPos[0]);
                faceNormal = Deki3D::TransformDirection(normalMatrix, faceNormal);
                if (faceNormal.LengthSquared() > 0.0f)
                    faceNormal.Normalize();
                const float d = -faceNormal.Dot(m_LightDir);
                const float lit = m_Config.ambient + (d > 0.0f ? d : 0.0f) * (1.0f - m_Config.ambient);
                cv[0].light = cv[1].light = cv[2].light = lit;
            }

            EmitTriangle(cv[0], cv[1], cv[2], materialIndex);
        }
    }
}

void Raster3D::EmitTriangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c,
                            uint16_t materialIndex)
{
    // Near-clip only. The left, right, top and bottom planes are handled by
    // clamping the screen bounding box, which is cheaper and just as correct
    // for a rasteriser that walks pixels rather than edges.
    const ClipVertex in[3] = { a, b, c };
    ClipVertex out[4];
    int outCount = 0;

    auto Dist = [](const ClipVertex& v) { return v.z + v.w; };  // near plane: z + w = 0

    for (int i = 0; i < 3; ++i)
    {
        const ClipVertex& cur = in[i];
        const ClipVertex& next = in[(i + 1) % 3];
        const float dCur = Dist(cur);
        const float dNext = Dist(next);

        if (dCur >= 0.0f)
            out[outCount++] = cur;

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
            v.light = cur.light + (next.light - cur.light) * t;
            v.color = cur.color;  // vertex colour is not interpolated across the cut
            out[outCount++] = v;
            if (outCount == 4)
                break;
        }
    }

    if (outCount >= 3)
        ProjectAndBin(out, outCount, materialIndex);
}

void Raster3D::ProjectAndBin(const ClipVertex* poly, int count, uint16_t materialIndex)
{
    ScreenVertex sv[4];
    const float halfW = static_cast<float>(m_Width) * 0.5f;
    const float halfH = static_cast<float>(m_Height) * 0.5f;

    for (int i = 0; i < count; ++i)
    {
        const ClipVertex& c = poly[i];
        const float invW = (c.w != 0.0f) ? 1.0f / c.w : 0.0f;
        sv[i].invW = invW;
        sv[i].x = (c.x * invW + 1.0f) * halfW;
        sv[i].y = (1.0f - c.y * invW) * halfH;  // clip space is Y up, the framebuffer is Y down
        sv[i].z = c.z * invW;
        sv[i].uOverW = c.u * invW;
        sv[i].vOverW = c.v * invW;
        sv[i].color = c.color;
        sv[i].light = c.light;

        if (m_Config.vertexSnap)
        {
            sv[i].x = std::floor(sv[i].x + 0.5f);
            sv[i].y = std::floor(sv[i].y + 0.5f);
        }
    }

    // Fan the clipped polygon back into triangles.
    for (int i = 1; i + 1 < count; ++i)
    {
        RasterTri tri;
        tri.v[0] = sv[0];
        tri.v[1] = sv[i];
        tri.v[2] = sv[i + 1];
        tri.materialIndex = materialIndex;

        const float area2 = (tri.v[1].x - tri.v[0].x) * (tri.v[2].y - tri.v[0].y) -
                            (tri.v[2].x - tri.v[0].x) * (tri.v[1].y - tri.v[0].y);

        // Front faces are counter-clockwise in world space, which is negative
        // signed area once Y points down.
        const Material3D& material = m_Materials[materialIndex];
        if (m_Config.backfaceCull && !material.doubleSided && area2 >= 0.0f)
            continue;
        if (area2 == 0.0f)
            continue;

        // The filler's inside test is "all three edge functions >= 0", which
        // needs positive area. Front faces are negative by the rule above, so
        // they are the ones that get flipped; a double-sided back face is
        // already positive and is left alone.
        if (area2 < 0.0f)
            std::swap(tri.v[1], tri.v[2]);

        ++m_Stats.trianglesClipped;
        Bin(tri);
    }
}

void Raster3D::Bin(const RasterTri& tri)
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

    int minX = std::max(0, static_cast<int>(std::floor(minXf)));
    int minY = std::max(0, static_cast<int>(std::floor(minYf)));
    int maxX = std::min(m_Width - 1, static_cast<int>(std::ceil(maxXf)));
    int maxY = std::min(m_Height - 1, static_cast<int>(std::ceil(maxYf)));
    if (minX > maxX || minY > maxY)
        return;

    const uint32_t index = static_cast<uint32_t>(m_Tris.size());
    m_Tris.push_back(tri);

    const int tile = m_Config.tileSize;
    const int t0x = minX / tile, t1x = maxX / tile;
    const int t0y = minY / tile, t1y = maxY / tile;
    for (int ty = t0y; ty <= t1y; ++ty)
    {
        for (int tx = t0x; tx <= t1x; ++tx)
        {
            m_Bins[static_cast<size_t>(ty) * m_TilesX + tx].push_back(index);
            ++m_Stats.trianglesBinned;
        }
    }
}

// ---------------------------------------------------------------------------
// Fill
// ---------------------------------------------------------------------------

void Raster3D::StartWorkers(int count)
{
    StopWorkers();
    if (count <= 0)
        return;
    m_StopWorkers = false;
    m_WorkerStats.assign(static_cast<size_t>(count), RasterStats{});
    m_WorkerHasWork.assign(static_cast<size_t>(count), 0);
    // Every worker plus the calling thread. Set before any thread exists, so
    // no worker ever reads a container the main thread is still growing.
    m_FillStride = count + 1;
    m_WorkersBusy = 0;
    m_Workers.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        m_Workers.emplace_back([this, i] { WorkerLoop(i); });
}

void Raster3D::StopWorkers()
{
    if (m_Workers.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_WorkMutex);
        m_StopWorkers = true;
    }
    m_WorkReady.notify_all();
    for (std::thread& worker : m_Workers)
        worker.join();
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
            m_WorkReady.wait(lock, [this, index] {
                return m_StopWorkers || m_WorkerHasWork[static_cast<size_t>(index)] != 0;
            });
            if (m_StopWorkers)
                return;
            m_WorkerHasWork[static_cast<size_t>(index)] = 0;
        }

        // Outside the lock: this is the whole point. Worker `index` takes
        // every stride-th tile, and touches nothing another worker touches.
        // The stride is a member fixed before any worker starts, rather than
        // m_Workers.size(), which the main thread is still appending to.
        FillTileRange(index, m_FillStride, m_TileDepth[index].data(), m_WorkerStats[index]);

        {
            std::lock_guard<std::mutex> lock(m_WorkMutex);
            if (--m_WorkersBusy == 0)
                m_WorkDone.notify_one();
        }
    }
}

void Raster3D::EndFrame()
{
    if (!m_Buffer)
        return;

    const int tileCount = m_TilesX * m_TilesY;
    const int threads = static_cast<int>(m_TileDepth.size());

    if (threads <= 1 || tileCount <= 1 || m_Workers.empty())
    {
        FillTileRange(0, 1, m_TileDepth[0].data(), m_Stats);
        return;
    }

    // Interleaved rather than contiguous: geometry clusters, so handing each
    // thread a solid block of the screen gives one of them every empty tile
    // and another all the work. Striding spreads a cluster across all of them.
    const int workerCount = static_cast<int>(m_Workers.size());
    for (RasterStats& s : m_WorkerStats)
        s.Reset();

    {
        std::lock_guard<std::mutex> lock(m_WorkMutex);
        m_WorkersBusy = workerCount;
        for (char& hasWork : m_WorkerHasWork)
            hasWork = 1;
    }
    m_WorkReady.notify_all();

    // The calling thread takes the last share rather than idling.
    FillTileRange(workerCount, m_FillStride, m_TileDepth[workerCount].data(), m_Stats);

    {
        std::unique_lock<std::mutex> lock(m_WorkMutex);
        m_WorkDone.wait(lock, [this] { return m_WorkersBusy == 0; });
    }

    for (const RasterStats& s : m_WorkerStats)
    {
        m_Stats.pixelsTested += s.pixelsTested;
        m_Stats.pixelsWritten += s.pixelsWritten;
    }
}

Raster3D::~Raster3D()
{
    StopWorkers();
}

void Raster3D::FillTileRange(int start, int stride, uint16_t* tileDepth, RasterStats& stats)
{
    const int tileCount = m_TilesX * m_TilesY;
    for (int index = start; index < tileCount; index += stride)
        FillTile(index % m_TilesX, index / m_TilesX, tileDepth, stats);
}

void Raster3D::FillTile(int tileX, int tileY, uint16_t* tileDepth, RasterStats& stats)
{
    const std::vector<uint32_t>& bin = m_Bins[static_cast<size_t>(tileY) * m_TilesX + tileX];
    if (bin.empty())
        return;

    const int tile = m_Config.tileSize;
    const int originX = tileX * tile;
    const int originY = tileY * tile;
    const int maxX = std::min(originX + tile, m_Width) - 1;
    const int maxY = std::min(originY + tile, m_Height) - 1;

    // Reset this thread's scratch buffer and reuse it for every tile it takes.
    std::fill(tileDepth, tileDepth + static_cast<size_t>(tile) * tile,
              static_cast<uint16_t>(kDepthMax));

    for (uint32_t index : bin)
        FillTriangleInTile(m_Tris[index], originX, originY, maxX, maxY, tileDepth, originX,
                           originY, stats);
}

void Raster3D::FillTriangleInTile(const RasterTri& tri, int minX, int minY, int maxX, int maxY,
                                  uint16_t* tileDepth, int tileOriginX, int tileOriginY,
                                  RasterStats& stats)
{
    const ScreenVertex& v0 = tri.v[0];
    const ScreenVertex& v1 = tri.v[1];
    const ScreenVertex& v2 = tri.v[2];

    // Clamp the walk to where this triangle actually is inside this tile.
    const float loX = std::min(v0.x, std::min(v1.x, v2.x));
    const float hiX = std::max(v0.x, std::max(v1.x, v2.x));
    const float loY = std::min(v0.y, std::min(v1.y, v2.y));
    const float hiY = std::max(v0.y, std::max(v1.y, v2.y));
    int bMinX = std::max(minX, static_cast<int>(std::floor(loX)));
    int bMaxX = std::min(maxX, static_cast<int>(std::ceil(hiX)));
    int bMinY = std::max(minY, static_cast<int>(std::floor(loY)));
    int bMaxY = std::min(maxY, static_cast<int>(std::ceil(hiY)));
    if (bMinX > bMaxX || bMinY > bMaxY)
        return;

    // floor+cast rather than lrint: lrint honours the current rounding mode,
    // so it stays a libm call, while floor is one instruction.
    auto Fixed = [](float f) { return static_cast<int32_t>(std::floor(f * kSubScale + 0.5f)); };
    const int32_t X0 = Fixed(v0.x), Y0 = Fixed(v0.y);
    const int32_t X1 = Fixed(v1.x), Y1 = Fixed(v1.y);
    const int32_t X2 = Fixed(v2.x), Y2 = Fixed(v2.y);

    // Edge functions, evaluated at the centre of the first pixel and stepped
    // by adds from there. One pixel is kSubScale in 28.4.
    const int32_t A01 = Y0 - Y1, B01 = X1 - X0;
    const int32_t A12 = Y1 - Y2, B12 = X2 - X1;
    const int32_t A20 = Y2 - Y0, B20 = X0 - X2;

    const int32_t Px = bMinX * kSubScale + kSubScale / 2;
    const int32_t Py = bMinY * kSubScale + kSubScale / 2;

    auto Edge = [](int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) -> int64_t {
        return static_cast<int64_t>(bx - ax) * (py - ay) - static_cast<int64_t>(by - ay) * (px - ax);
    };

    int64_t rowE0 = Edge(X1, Y1, X2, Y2, Px, Py);  // weight of v0
    int64_t rowE1 = Edge(X2, Y2, X0, Y0, Px, Py);  // weight of v1
    int64_t rowE2 = Edge(X0, Y0, X1, Y1, Px, Py);  // weight of v2

    // dE/dpx is A, dE/dpy is B, both scaled by one pixel in 28.4. E0 is the
    // edge opposite v0, so it steps with the v1->v2 edge, and so on round.
    const int64_t stepX0 = static_cast<int64_t>(A12) * kSubScale;
    const int64_t stepY0 = static_cast<int64_t>(B12) * kSubScale;
    const int64_t stepX1 = static_cast<int64_t>(A20) * kSubScale;
    const int64_t stepY1 = static_cast<int64_t>(B20) * kSubScale;
    const int64_t stepX2 = static_cast<int64_t>(A01) * kSubScale;
    const int64_t stepY2 = static_cast<int64_t>(B01) * kSubScale;

    const float area2 = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (area2 == 0.0f)
        return;
    const float invArea = 1.0f / area2;

    const Gradient gInvW = MakeGradient(v0.invW, v1.invW, v2.invW,
                                        v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea);
    const Gradient gZ = MakeGradient(v0.z, v1.z, v2.z,
                                     v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea);
    // Perspective-correct interpolates u/w and v/w, which are linear in screen
    // space, and divides back at the span endpoints. Affine interpolates u and
    // v themselves, which are not linear in screen space: that error IS the
    // PlayStation-era warp, and it grows with how much screen a triangle covers.
    const bool affine = !m_Config.perspectiveCorrect;
    auto VertexU = [](const ScreenVertex& v) { return v.invW != 0.0f ? v.uOverW / v.invW : 0.0f; };
    auto VertexV = [](const ScreenVertex& v) { return v.invW != 0.0f ? v.vOverW / v.invW : 0.0f; };

    const Gradient gU =
        affine ? MakeGradient(VertexU(v0), VertexU(v1), VertexU(v2),
                              v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea)
               : MakeGradient(v0.uOverW, v1.uOverW, v2.uOverW,
                              v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea);
    const Gradient gV =
        affine ? MakeGradient(VertexV(v0), VertexV(v1), VertexV(v2),
                              v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea)
               : MakeGradient(v0.vOverW, v1.vOverW, v2.vOverW,
                              v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, invArea);

    const Material3D& mat = m_Materials[tri.materialIndex];
    const Texture3D* tex = mat.texture;
    const bool textured = tex && tex->Valid();
    const int uMask = textured ? (tex->width - 1) : 0;
    const int vMask = textured ? (tex->height - 1) : 0;
    const int wShift = textured ? tex->widthShift : 0;

    // Flat colour when there is no texture: vertex colour of the first vertex,
    // modulated by the material tint. Per-vertex colour interpolation is a
    // later refinement; nothing in the pipeline needs it yet.
    const uint32_t tint = mat.tint;
    const uint16_t flatColor = To565(((v0.color & 0xFF) * (tint & 0xFF) / 255) |
                                     ((((v0.color >> 8) & 0xFF) * ((tint >> 8) & 0xFF) / 255) << 8) |
                                     ((((v0.color >> 16) & 0xFF) * ((tint >> 16) & 0xFF) / 255) << 16));

    const bool lit = mat.shading != ShadingModel::Unlit;
    const int subdiv = m_Config.perspectiveCorrect ? m_Config.spanSubdivision : (bMaxX - bMinX + 1);
    const int tileStride = m_Config.tileSize;

    for (int py = bMinY; py <= bMaxY; ++py)
    {
        int64_t e0 = rowE0, e1 = rowE1, e2 = rowE2;

        // Find the run of covered pixels on this scanline rather than testing
        // every pixel of the bounding box.
        int spanStart = -1;
        for (int px = bMinX; px <= bMaxX; ++px)
        {
            const bool inside = (e0 >= 0) && (e1 >= 0) && (e2 >= 0);
            if (inside && spanStart < 0)
                spanStart = px;
            if ((!inside || px == bMaxX) && spanStart >= 0)
            {
                const int spanEnd = inside ? px : px - 1;

                // --- one covered run, walked in subdivided spans ---
                const float fy = static_cast<float>(py) + 0.5f - v0.y;
                int runX = spanStart;
                while (runX <= spanEnd)
                {
                    const int chunkEnd = std::min(runX + subdiv - 1, spanEnd);
                    const float fx0 = static_cast<float>(runX) + 0.5f - v0.x;
                    const float fx1 = static_cast<float>(chunkEnd) + 0.5f - v0.x;

                    // The only divides in the whole loop: two per span, and
                    // none at all on the affine path.
                    float uStart, vStart, uEnd, vEnd;
                    if (affine)
                    {
                        uStart = Eval(gU, fx0, fy);
                        vStart = Eval(gV, fx0, fy);
                        uEnd = Eval(gU, fx1, fy);
                        vEnd = Eval(gV, fx1, fy);
                    }
                    else
                    {
                        const float invW0 = Eval(gInvW, fx0, fy);
                        const float invW1 = Eval(gInvW, fx1, fy);
                        const float w0 = (invW0 != 0.0f) ? 1.0f / invW0 : 0.0f;
                        const float w1 = (invW1 != 0.0f) ? 1.0f / invW1 : 0.0f;
                        uStart = Eval(gU, fx0, fy) * w0;
                        vStart = Eval(gV, fx0, fy) * w0;
                        uEnd = Eval(gU, fx1, fy) * w1;
                        vEnd = Eval(gV, fx1, fy) * w1;
                    }

                    const int chunkLen = chunkEnd - runX + 1;
                    const float inv = (chunkLen > 1) ? 1.0f / static_cast<float>(chunkLen - 1) : 0.0f;

                    int32_t u = 0, v = 0, du = 0, dv = 0;
                    if (textured)
                    {
                        u = static_cast<int32_t>(uStart * tex->width * (1 << kUvFrac));
                        v = static_cast<int32_t>(vStart * tex->height * (1 << kUvFrac));
                        const int32_t uE = static_cast<int32_t>(uEnd * tex->width * (1 << kUvFrac));
                        const int32_t vE = static_cast<int32_t>(vEnd * tex->height * (1 << kUvFrac));
                        du = (chunkLen > 1) ? static_cast<int32_t>((uE - u) * inv) : 0;
                        dv = (chunkLen > 1) ? static_cast<int32_t>((vE - v) * inv) : 0;
                    }

                    // Depth, also stepped with adds.
                    const float z0 = Eval(gZ, fx0, fy);
                    const float z1 = Eval(gZ, fx1, fy);
                    auto ToDepth = [](float ndcZ) -> int32_t {
                        float d = (ndcZ * 0.5f + 0.5f) * static_cast<float>(kDepthMax);
                        if (d < 0.0f) d = 0.0f;
                        if (d > static_cast<float>(kDepthMax)) d = static_cast<float>(kDepthMax);
                        return static_cast<int32_t>(d * (1 << kDepthFrac));
                    };
                    int32_t z = ToDepth(z0);
                    const int32_t zE = ToDepth(z1);
                    const int32_t dz = (chunkLen > 1) ? static_cast<int32_t>((zE - z) * inv) : 0;

                    const uint32_t shade =
                        lit ? static_cast<uint32_t>(std::min(32.0f, std::max(0.0f, v0.light * 32.0f))) : 32u;

                    // Row bases, so the loop below indexes by x alone: the
                    // multiply that turned a pixel into a tile offset was
                    // costing more than the depth test it fed.
                    uint16_t* const fbRow = reinterpret_cast<uint16_t*>(m_Buffer) +
                                            static_cast<size_t>(py) * m_Width;
                    uint16_t* const depthRow =
                        tileDepth + static_cast<size_t>(py - tileOriginY) * tileStride - tileOriginX;
                    uint32_t written = 0;

                    // ---- the inner loop: integer only ----
                    for (int x = runX; x <= chunkEnd; ++x)
                    {
                        const int32_t depth = z >> kDepthFrac;

                        if (!m_Config.depthTest || depth < static_cast<int32_t>(depthRow[x]))
                        {
                            uint16_t color = flatColor;
                            bool write = true;

                            if (textured)
                            {
                                const int tu = (u >> kUvFrac) & uMask;
                                const int tv = (v >> kUvFrac) & vMask;
                                const int texel = (tv << wShift) + tu;
                                if (tex->palette)
                                {
                                    const uint8_t idx = tex->pixels[texel];
                                    if (mat.alphaTest && idx == 0)
                                        write = false;
                                    else
                                        color = tex->palette[idx];
                                }
                                else
                                {
                                    color = reinterpret_cast<const uint16_t*>(tex->pixels)[texel];
                                    if (mat.alphaTest && color == 0xF81F)  // magenta is the hole
                                        write = false;
                                }
                            }

                            if (write)
                            {
                                if (lit)
                                    color = Shade565(color, shade);
                                fbRow[x] = color;
                                if (m_Config.depthWrite)
                                    depthRow[x] = static_cast<uint16_t>(depth);
                                ++written;
                            }
                        }

                        u += du;
                        v += dv;
                        z += dz;
                    }

                    stats.pixelsTested += static_cast<uint32_t>(chunkEnd - runX + 1);
                    stats.pixelsWritten += written;
                    runX = chunkEnd + 1;
                }

                spanStart = -1;
                if (!inside)
                    ;  // the run ended here; keep scanning for another
            }

            e0 += stepX0;
            e1 += stepX1;
            e2 += stepX2;
        }

        rowE0 += stepY0;
        rowE1 += stepY1;
        rowE2 += stepY2;
    }
}

}  // namespace Deki3D
