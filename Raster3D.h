#pragma once

// Tiled software triangle rasteriser.
//
// Draws into whatever framebuffer it is handed and uses only the engine's
// maths types and colour format enum, so it can be benchmarked and unit
// tested without a scene, a camera or an asset.
//
// How a frame works:
//   BeginFrame  target, size, format, configuration
//   DrawMesh    per mesh: transform, near-clip, project, bin into tiles
//   EndFrame    per tile: clear its depth, fill the triangles binned to it
//
// Binning first and filling per tile keeps the depth buffer small. A
// full-screen 16-bit depth buffer at 320x240 is 150 KB, which a plain ESP32
// cannot spare; one 32x32 tile is 2 KB, reused across the frame. On a desktop
// the tile's depth and colour working set stays inside L1.
//
// A frame of more than RasterConfig::batchTriangles is filled in batches, so
// a mesh of any size draws in fixed memory: when the batch is full its tiles
// are filled and binning starts again. Later batches must depth-test against
// earlier ones, so from the first such fill the frame uses one depth buffer
// for the whole screen, in PSRAM where the board has it. The triangles and
// tile lists are there too (RegionArray): a std::vector would take a board's
// small internal heap, and abort it when that is full.
//
// Float does the per-vertex work: transform, clipping, projection, and the
// perspective divide at span endpoints. Fixed point does the per-pixel work:
// edge functions, depth, and attribute stepping. That suits a span loop on
// any hardware, and chips without an FPU pay float emulation only at vertex
// rate.

#include <deki/Engine.h>  // Deki::ColorFormat
#include <deki/Vector.h>
#include <deki/providers/Memory.h>

#include "Mesh3D.h"
#include "Mesh3DBackend.h"  // RasterConfig, RasterStats

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace Deki3D
{

class Raster3D : public Mesh3DBackend
{
public:
    Raster3D() = default;
    ~Raster3D() override;

    // Non-copyable: it owns worker threads.
    Raster3D(const Raster3D&) = delete;
    Raster3D& operator=(const Raster3D&) = delete;

    /// Targets a framebuffer and starts collecting geometry. The buffer is
    /// not cleared: the caller owns the background, as in the 2D path.
    void BeginFrame(uint8_t* buffer, int32_t width, int32_t height, Deki::ColorFormat format,
                    const RasterConfig& config) override;

    /// Transforms, clips and bins one mesh. `mvp` takes object space to clip
    /// space; `normalMatrix` normals and `model` positions to world space,
    /// read only by the lit shading models. A mesh is lit by the
    /// kLightsPerMesh lights that reach it most.
    void DrawMesh(const Mesh3D& mesh, const Material3D* materials, uint16_t materialCount, const Deki::Mat4& mvp,
                  const Deki::Mat4& normalMatrix, const Deki::Mat4& model = Deki::Mat4::Identity()) override;

    static constexpr int kLightsPerMesh = 4;

    /// The most memory a frame at `width` x `height` takes beyond the meshes
    /// themselves: a full batch of triangles, their tile lists, the
    /// whole-screen depth buffer of a frame past one batch, and each fill
    /// thread's tile depth. What a board's build counts for its 3D.
    static size_t WorkingBytes(int32_t width, int32_t height, const RasterConfig& config);

    /// Fills every tile. Nothing reaches the framebuffer before this, but in a
    /// frame of more than one batch, which fills its earlier batches as
    /// DrawMesh fills them up.
    void EndFrame() override;

    /// The frame's clear, one pixel of its format, for the frame's first fill
    /// to draw under its triangles, tile by tile, tiles without a triangle
    /// included: the caller then makes no clear of its own
    /// (QuadBlit::TakePendingClear). Set after BeginFrame, before anything is
    /// filled.
    void SetBackground(const uint8_t* pixel);

    const RasterStats& Stats() const { return m_Stats; }
    const RasterConfig& Config() const { return m_Config; }

private:
    // A vertex after projection, in screen pixels, with the attributes the
    // span loop interpolates. Perspective correction uses uOverW, vOverW and
    // invW because they are linear in screen space; u and v are not.
    struct ScreenVertex
    {
        float x, y;
        float invW;
        float z;  // normalised device z in [-1, 1]
        float uOverW, vOverW;
        uint32_t color;
        float lightR, lightG, lightB;
    };

    struct RasterTri
    {
        ScreenVertex v[3];
        // An index into m_Materials, not a pointer to the caller's material:
        // filling waits until EndFrame, when the caller's stack copy is gone.
        uint16_t materialIndex;
    };

    // Clip space, before the divide. Sutherland-Hodgman against the near
    // plane works here because that plane is z + w = 0.
    struct ClipVertex
    {
        float x, y, z, w;
        float u, v;
        uint32_t color;
        float lightR, lightG, lightB;
    };

    void EmitTriangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c, uint16_t materialIndex);
    /// `c` in screen pixels, with its attributes.
    ScreenVertex Project(const ClipVertex& c) const;

    // The vertices DrawMesh transformed most recently, by index: a vertex is
    // shared by about six triangles, and neighbouring triangles mostly come
    // close together, so most corners are found here rather than read from
    // the mesh (PSRAM on a board) and transformed again. Lit only when a
    // front-facing triangle needs it. Emptied at each submesh, whose
    // material decides the lighting.
    struct CachedVertex
    {
        uint32_t index = UINT32_MAX;
        bool lit = false;
        // In front of the near plane, so a triangle of three such corners
        // needs no clip and takes `screen`, its projection, as it is.
        bool inFront = false;
        ClipVertex clip;
        ScreenVertex screen;     // its colour and light are the clip's, set when it is emitted
        Deki::Vector3 position;  // object space, for the lighting
        Deki::Vector3 normal;
    };
    static constexpr int kVertexCacheSize = 128;  // a power of two
    std::vector<CachedVertex> m_VertexCache;
    void ProjectAndBin(const ClipVertex* poly, int count, uint16_t materialIndex);
    /// A triangle of three cached corners, with the clip vertices that carry
    /// their light: projected from the cache when no corner is behind the
    /// near plane, and clipped first otherwise.
    void EmitCorners(const CachedVertex* const corner[3], const ClipVertex& a, const ClipVertex& b, const ClipVertex& c,
                     uint16_t materialIndex);
    /// Fans a projected polygon into triangles and bins the ones that face
    /// the camera, or are double-sided.
    void BinFan(ScreenVertex* sv, int count, uint16_t materialIndex);
    void Bin(const RasterTri& tri);
    /// Fills every `stride`-th tile of the row-major grid starting at `start`,
    /// using `tileDepth` as scratch depth buffer and adding to `stats`. Both
    /// are per thread, which makes concurrent calls safe.
    void FillTileRange(int start, int stride, int thread, RasterStats& stats);
    void FillTile(int tileX, int tileY, int thread, RasterStats& stats);
    /// `depth` is the depth of pixel (tileOriginX, tileOriginY), rows
    /// `depthStride` apart: the tile's scratch buffer, or the frame's.
    void FillTriangleInTile(const RasterTri& tri, int minX, int minY, int maxX, int maxY, uint16_t* depth,
                            int depthStride, int tileOriginX, int tileOriginY, RasterStats& stats);
    /// Fills every tile from the triangles binned so far.
    void FillAll();
    /// Fills the batch so far and empties it, keeping the frame's depth: false
    /// when the whole-screen depth buffer cannot be had, and the frame stops
    /// taking triangles.
    bool FlushBatch();

    /// A growing array in Memory::External, which on a board without PSRAM
    /// is internal RAM: Reserve says when it cannot grow, where a std::vector
    /// would abort the board. For trivially copyable T. Its capacity is kept
    /// from frame to frame.
    template <typename T>
    class RegionArray
    {
    public:
        RegionArray() = default;
        RegionArray(const RegionArray&) = delete;
        RegionArray& operator=(const RegionArray&) = delete;
        RegionArray(RegionArray&& other) noexcept { *this = std::move(other); }
        RegionArray& operator=(RegionArray&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                m_Data = other.m_Data;
                m_Count = other.m_Count;
                m_Capacity = other.m_Capacity;
                other.m_Data = nullptr;
                other.m_Count = other.m_Capacity = 0;
            }
            return *this;
        }
        ~RegionArray() { Release(); }

        /// Room for `count` in all, growing to at least twice the capacity.
        bool Reserve(size_t count)
        {
            if (count <= m_Capacity)
            {
                return true;
            }
            size_t capacity = m_Capacity < 16 ? 16 : m_Capacity * 2;
            if (capacity < count)
            {
                capacity = count;
            }
            T* data = Deki::Memory::AllocateArray<T>(capacity, Deki::Memory::External);
            if (!data)
            {
                return false;
            }
            const size_t kept = m_Count;
            if (kept > 0)
            {
                std::memcpy(data, m_Data, kept * sizeof(T));
            }
            Release();  // which empties it: the count is set again below
            m_Data = data;
            m_Count = kept;
            m_Capacity = capacity;
            return true;
        }
        /// After Reserve made the room.
        void PushReserved(const T& value) { m_Data[m_Count++] = value; }
        void Clear() { m_Count = 0; }
        size_t Size() const { return m_Count; }
        bool Empty() const { return m_Count == 0; }
        const T& operator[](size_t i) const { return m_Data[i]; }
        const T* begin() const { return m_Data; }
        const T* end() const { return m_Data + m_Count; }

    private:
        void Release()
        {
            if (m_Data)
            {
                Deki::Memory::Free(m_Data);
            }
            m_Data = nullptr;
            m_Count = 0;
            m_Capacity = 0;
        }
        T* m_Data = nullptr;
        size_t m_Count = 0;
        size_t m_Capacity = 0;
    };

    uint8_t* m_Buffer = nullptr;
    // SetBackground's pixel, until the fill that writes it.
    bool m_HasBackground = false;
    uint8_t m_Background[4] = {};
    void FillBackground(int originX, int originY, int maxX, int maxY);
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    Deki::ColorFormat m_Format = Deki::ColorFormat::RGB565;
    size_t m_BytesPerPixel = 2;  // of m_Format
    RasterConfig m_Config;  // its lights' directions normalised
    // The lights of the mesh DrawMesh is on: the frame's that reach it most.
    Light3D m_MeshLights[kLightsPerMesh];
    int m_MeshLightCount = 0;
    void PickLights(const Mesh3D& mesh, const Deki::Mat4& model);

    int m_TilesX = 0;
    int m_TilesY = 0;

    // Materials copied from each DrawMesh call, so a caller's materials need
    // only live for the call.
    std::vector<Material3D> m_Materials;
    RegionArray<RasterTri> m_Tris;
    std::vector<RegionArray<uint32_t>> m_Bins;  // one list of triangle indices per tile

    // The frame's depth once it is filled in batches: width x height, kept
    // allocated for the next such frame. m_FrameDepthActive says this frame
    // uses it.
    uint16_t* m_FrameDepth = nullptr;
    size_t m_FrameDepthCells = 0;
    bool m_FrameDepthActive = false;
    // Set when another batch could not be had: the rest of the frame's
    // triangles are counted as dropped, and said once.
    bool m_FrameFull = false;
    bool m_DropReported = false;
    // One scratch depth buffer per fill thread, reused for every tile that
    // thread takes: 2 KB at a 32-pixel tile, against 150 KB for full screen.
    std::vector<std::vector<uint16_t>> m_TileDepth;

    RasterStats m_Stats;

    // --- the fill workers ---------------------------------------------------
    //
    // Created once and parked on a condition variable between frames.
    // Creating a thread costs tens of microseconds, more than the whole fill
    // of a 320x240 frame, so spawning per frame is slower than one thread.
    //
    // Workers run only FillTileRange, and each tile writes only its own
    // framebuffer pixels and its own scratch depth buffer, so the fill needs
    // no locking. The lock is for handing out a frame and learning when it is
    // finished.
    void StartWorkers(int count);
    void StopWorkers();
    void WorkerLoop(int index);

    std::vector<std::thread> m_Workers;
    std::mutex m_WorkMutex;
    std::condition_variable m_WorkReady;
    std::condition_variable m_WorkDone;
    std::vector<RasterStats> m_WorkerStats;
    // One flag per worker, raised by the issuing side and lowered only by its
    // own worker. Must not become a frame counter: a worker created while
    // frames run would need a starting value, and any value races with the
    // next frame.
    std::vector<char> m_WorkerHasWork;
    int m_FillStride = 1;  // fixed before the workers start; never read from m_Workers
    int m_WorkersBusy = 0;
    bool m_StopWorkers = false;
};

}  // namespace Deki3D
