#include "Mesh3DBackend.h"

#include <atomic>

namespace Deki3D
{
namespace
{
const GpuBackendProvider* s_Provider = nullptr;
uint32_t s_Generation = 0;
std::atomic<uint32_t> s_Revision{ 0 };
}  // namespace

void SetGpuBackendProvider(const GpuBackendProvider* provider)
{
    s_Provider = provider;
    ++s_Generation;
}

const GpuBackendProvider* GetGpuBackendProvider()
{
    return s_Provider;
}

uint32_t GpuBackendGeneration()
{
    return s_Generation;
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
