#include "MeshSimplify.h"

#ifdef DEKI_EDITOR

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace Deki3D
{
namespace
{

struct Vec3d
{
    double x = 0, y = 0, z = 0;
};

Vec3d Sub(const Vec3d& a, const Vec3d& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

Vec3d Cross(const Vec3d& a, const Vec3d& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

double Dot(const Vec3d& a, const Vec3d& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// The sum of squared distances to a set of planes, as a symmetric 4x4
/// matrix: its upper triangle, a b c d / e f g / h i / j.
struct Quadric
{
    double m[10] = {};

    void AddPlane(double a, double b, double c, double d, double weight)
    {
        m[0] += weight * a * a;
        m[1] += weight * a * b;
        m[2] += weight * a * c;
        m[3] += weight * a * d;
        m[4] += weight * b * b;
        m[5] += weight * b * c;
        m[6] += weight * b * d;
        m[7] += weight * c * c;
        m[8] += weight * c * d;
        m[9] += weight * d * d;
    }

    void Add(const Quadric& o)
    {
        for (int i = 0; i < 10; ++i)
        {
            m[i] += o.m[i];
        }
    }

    double Error(const Vec3d& p) const
    {
        const double x = p.x, y = p.y, z = p.z;
        return m[0] * x * x + 2 * m[1] * x * y + 2 * m[2] * x * z + 2 * m[3] * x + m[4] * y * y + 2 * m[5] * y * z +
               2 * m[6] * y + m[7] * z * z + 2 * m[8] * z + m[9];
    }
};

/// Moving `from` onto `to`, worth `cost`. Stale once either vertex has
/// changed since: their stamps no longer match.
struct Collapse
{
    double cost;
    uint32_t from, to;
    uint32_t fromStamp, toStamp;

    bool operator>(const Collapse& o) const { return cost > o.cost; }
};

uint64_t EdgeKey(uint32_t a, uint32_t b)
{
    return a < b ? (static_cast<uint64_t>(a) << 32) | b : (static_cast<uint64_t>(b) << 32) | a;
}

}  // namespace

void SimplifyMesh(const std::vector<float>& positions, std::vector<SimplifyTriangle>& triangles,
                  uint32_t targetTriangles)
{
    const size_t vertexCount = positions.size() / 3;
    if (triangles.size() <= targetTriangles || vertexCount == 0)
    {
        return;
    }
    auto P = [&](uint32_t v) { return Vec3d{ positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2] }; };

    // Which vertices may move. A vertex on an edge only one triangle uses (an
    // open edge, or a seam, since the triangles across a seam use the other
    // vertex at that place) stays, as does one whose triangles are in two
    // materials.
    std::vector<uint8_t> locked(vertexCount, 0);
    {
        std::unordered_map<uint64_t, uint32_t> edgeUse;
        edgeUse.reserve(triangles.size() * 3);
        for (const SimplifyTriangle& t : triangles)
        {
            for (int k = 0; k < 3; ++k)
            {
                ++edgeUse[EdgeKey(t.v[k], t.v[(k + 1) % 3])];
            }
        }
        for (const auto& [key, uses] : edgeUse)
        {
            if (uses == 1)
            {
                locked[key >> 32] = 1;
                locked[key & 0xFFFFFFFFu] = 1;
            }
        }
        std::vector<uint32_t> firstSubmesh(vertexCount, UINT32_MAX);
        for (const SimplifyTriangle& t : triangles)
        {
            for (uint32_t v : t.v)
            {
                if (firstSubmesh[v] == UINT32_MAX)
                {
                    firstSubmesh[v] = t.submesh;
                }
                else if (firstSubmesh[v] != t.submesh)
                {
                    locked[v] = 1;
                }
            }
        }
    }

    // Each vertex's quadric: the planes of its triangles, by area.
    std::vector<Quadric> quadrics(vertexCount);
    std::vector<std::vector<uint32_t>> around(vertexCount);
    // Each triangle's facing as the model has it, unit length (zero for one
    // with no area).
    std::vector<Vec3d> facing(triangles.size());
    for (uint32_t ti = 0; ti < triangles.size(); ++ti)
    {
        const SimplifyTriangle& t = triangles[ti];
        const Vec3d a = P(t.v[0]), b = P(t.v[1]), c = P(t.v[2]);
        Vec3d n = Cross(Sub(b, a), Sub(c, a));
        const double len = std::sqrt(Dot(n, n));
        if (len > 0.0)
        {
            n = { n.x / len, n.y / len, n.z / len };
            facing[ti] = n;
            const double area = len * 0.5;
            for (uint32_t v : t.v)
            {
                quadrics[v].AddPlane(n.x, n.y, n.z, -Dot(n, a), area);
            }
        }
        for (uint32_t v : t.v)
        {
            around[v].push_back(ti);
        }
    }

    std::vector<uint8_t> dead(triangles.size(), 0);
    std::vector<uint32_t> stamp(vertexCount, 0);
    std::priority_queue<Collapse, std::vector<Collapse>, std::greater<Collapse>> queue;
    auto offer = [&](uint32_t from, uint32_t to)
    {
        if (!locked[from] && from != to)
        {
            Quadric q = quadrics[from];
            q.Add(quadrics[to]);
            queue.push({ q.Error(P(to)), from, to, stamp[from], stamp[to] });
        }
    };
    for (const SimplifyTriangle& t : triangles)
    {
        for (int k = 0; k < 3; ++k)
        {
            offer(t.v[k], t.v[(k + 1) % 3]);
            offer(t.v[(k + 1) % 3], t.v[k]);
        }
    }

    // Whether moving `from` onto `to` keeps every triangle that stays the
    // way up it is, with some area, and still facing within 84 degrees of the
    // way the model has it: each collapse alone may turn a triangle less, but
    // many in a row could otherwise turn it right over.
    auto keepsShape = [&](uint32_t from, uint32_t to)
    {
        const Vec3d target = P(to);
        for (uint32_t ti : around[from])
        {
            const SimplifyTriangle& t = triangles[ti];
            if (dead[ti] || t.v[0] == to || t.v[1] == to || t.v[2] == to)
            {
                continue;
            }
            Vec3d before[3], after[3];
            for (int k = 0; k < 3; ++k)
            {
                before[k] = P(t.v[k]);
                after[k] = t.v[k] == from ? target : before[k];
            }
            const Vec3d n0 = Cross(Sub(before[1], before[0]), Sub(before[2], before[0]));
            const Vec3d n1 = Cross(Sub(after[1], after[0]), Sub(after[2], after[0]));
            const double l0 = std::sqrt(Dot(n0, n0)), l1 = std::sqrt(Dot(n1, n1));
            if (l1 <= l0 * 1e-6 || Dot(n0, n1) < 0.2 * l0 * l1 ||
                (Dot(facing[ti], facing[ti]) > 0.0 && Dot(n1, facing[ti]) < 0.1 * l1))
            {
                return false;
            }
        }
        return true;
    };

    // Whether `from` and `to` are still an edge that can collapse cleanly:
    // some triangle has both, and the vertices next to both are exactly the
    // third corners of those triangles. Another shared neighbour would fold
    // the surface onto itself (the link condition).
    std::vector<uint32_t> fromNear, toNear, opposite;
    auto neighbours = [&](uint32_t v, std::vector<uint32_t>& out)
    {
        out.clear();
        for (uint32_t ti : around[v])
        {
            if (!dead[ti])
            {
                for (uint32_t w : triangles[ti].v)
                {
                    if (w != v)
                    {
                        out.push_back(w);
                    }
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    };
    auto isEdge = [&](uint32_t from, uint32_t to)
    {
        opposite.clear();
        for (uint32_t ti : around[from])
        {
            const SimplifyTriangle& t = triangles[ti];
            if (!dead[ti] && (t.v[0] == to || t.v[1] == to || t.v[2] == to))
            {
                for (uint32_t w : t.v)
                {
                    if (w != from && w != to)
                    {
                        opposite.push_back(w);
                    }
                }
            }
        }
        if (opposite.empty())
        {
            return false;
        }
        std::sort(opposite.begin(), opposite.end());
        neighbours(from, fromNear);
        neighbours(to, toNear);
        size_t shared = 0;
        for (size_t i = 0, j = 0; i < fromNear.size() && j < toNear.size();)
        {
            if (fromNear[i] < toNear[j])
            {
                ++i;
            }
            else if (toNear[j] < fromNear[i])
            {
                ++j;
            }
            else
            {
                if (fromNear[i] != to && !std::binary_search(opposite.begin(), opposite.end(), fromNear[i]))
                {
                    return false;
                }
                ++shared;
                ++i;
                ++j;
            }
        }
        return shared > 0;
    };

    size_t alive = triangles.size();
    while (alive > targetTriangles && !queue.empty())
    {
        const Collapse c = queue.top();
        queue.pop();
        if (c.fromStamp != stamp[c.from] || c.toStamp != stamp[c.to] || around[c.from].empty())
        {
            continue;
        }
        if (!isEdge(c.from, c.to) || !keepsShape(c.from, c.to))
        {
            continue;
        }

        // The triangles on the edge go; the rest of `from`'s move to `to`.
        for (uint32_t ti : around[c.from])
        {
            if (dead[ti])
            {
                continue;
            }
            SimplifyTriangle& t = triangles[ti];
            if (t.v[0] == c.to || t.v[1] == c.to || t.v[2] == c.to)
            {
                dead[ti] = 1;
                --alive;
                continue;
            }
            for (uint32_t& v : t.v)
            {
                if (v == c.from)
                {
                    v = c.to;
                }
            }
            around[c.to].push_back(ti);
        }
        around[c.from].clear();
        quadrics[c.to].Add(quadrics[c.from]);
        ++stamp[c.from];
        ++stamp[c.to];

        // Only the edges at `to` cost something new (its quadric grew), and
        // its new stamp has made their old offers stale.
        std::vector<uint32_t>& list = around[c.to];
        list.erase(std::remove_if(list.begin(), list.end(), [&](uint32_t ti) { return dead[ti] != 0; }), list.end());
        for (uint32_t ti : list)
        {
            for (uint32_t w : triangles[ti].v)
            {
                if (w != c.to)
                {
                    offer(w, c.to);
                    offer(c.to, w);
                }
            }
        }
    }

    std::vector<SimplifyTriangle> kept;
    kept.reserve(alive);
    for (size_t ti = 0; ti < triangles.size(); ++ti)
    {
        if (!dead[ti])
        {
            kept.push_back(triangles[ti]);
        }
    }
    triangles.swap(kept);
}

}  // namespace Deki3D

#endif  // DEKI_EDITOR
