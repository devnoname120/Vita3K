#pragma once

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace renderer::vulkan::diagnostics {

inline std::size_t parse_vertex_stream_ring_capacity(const char *value,
    unsigned default_mib, std::size_t bytes_per_mib) {
    if (!value || !*value)
        return default_mib * bytes_per_mib;

    unsigned capacity_mib = 0;
    const char *const end = value + std::strlen(value);
    const auto [parsed_end, error] = std::from_chars(value, end, capacity_mib);
    if (error != std::errc() || parsed_end != end || capacity_mib < default_mib || capacity_mib > 512)
        return default_mib * bytes_per_mib;

    return static_cast<std::size_t>(capacity_mib) * bytes_per_mib;
}

// VITA3K_VERTEX_STREAM_RING_MIB grows the Vulkan vertex-stream ring (64 to 512 MiB).
inline std::size_t vertex_stream_ring_capacity_bytes() {
    constexpr std::size_t bytes_per_mib = 1024u * 1024u;
    constexpr unsigned default_mib = 64;

    return parse_vertex_stream_ring_capacity(
        std::getenv("VITA3K_VERTEX_STREAM_RING_MIB"), default_mib, bytes_per_mib);
}

}
