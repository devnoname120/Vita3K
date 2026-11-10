#pragma once

#include <cstdlib>
#include <cstring>

namespace renderer::vulkan::diagnostics {

inline bool stream_ring_wrap_sync_enabled() {
    const char *const value = std::getenv("VITA3K_STREAM_RING_SYNC_ON_WRAP");
    return value && std::strcmp(value, "1") == 0;
}

}
