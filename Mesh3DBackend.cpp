#include "Mesh3DBackend.h"

#include <cmath>

#include <algorithm>
#include <atomic>

namespace Deki3D
{
namespace
{
std::atomic<uint32_t> s_Revision{ 0 };
}  // namespace

Deki::Vector3 LightAt(const Deki::Vector3& ambient, const Light3D* lights, int count, const Deki::Vector3& position,
                      const Deki::Vector3& normal)
{
    Deki::Vector3 sum = ambient;
    for (int i = 0; i < count; ++i)
    {
        const Light3D& l = lights[i];
        Deki::Vector3 toward;  // from the surface to the light, unit
        float share = 1.0f;
        if (l.kind == Light3D::Kind::Directional)
        {
            toward = Deki::Vector3(-l.direction.x, -l.direction.y, -l.direction.z);
        }
        else
        {
            toward = l.position - position;
            const float d2 = toward.LengthSquared();
            const float t = 1.0f - d2 / (l.range * l.range);
            if (t <= 0.0f)
            {
                continue;
            }
            share = t * t;
            if (d2 > 0.0f)
            {
                toward = toward * (1.0f / std::sqrt(d2));
            }
            if (l.kind == Light3D::Kind::Spot)
            {
                // How far off its facing the surface is, seen from the light.
                const float c = -toward.Dot(l.direction);
                const float s = std::min(std::max((c - l.cosOuter) / (l.cosInner - l.cosOuter), 0.0f), 1.0f);
                share *= s * s * (3.0f - 2.0f * s);
            }
        }
        const float facing = normal.Dot(toward);
        if (facing > 0.0f && share > 0.0f)
        {
            sum = sum + l.color * (facing * share);
        }
    }
    sum.x = std::min(sum.x, 2.0f);
    sum.y = std::min(sum.y, 2.0f);
    sum.z = std::min(sum.z, 2.0f);
    return sum;
}

uint32_t NextRevision()
{
    uint32_t revision = s_Revision.fetch_add(1) + 1;
    // Zero means untracked. A wrap after four billion loads is far away, but
    // skip it rather than hand out the one value that disables caching.
    if (revision == 0)
    {
        revision = s_Revision.fetch_add(1) + 1;
    }
    return revision;
}

}  // namespace Deki3D
