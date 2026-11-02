#include "Mesh3DBackend.h"

#include <atomic>

namespace Deki3D
{
namespace
{
std::atomic<uint32_t> s_Revision{ 0 };
}  // namespace

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
