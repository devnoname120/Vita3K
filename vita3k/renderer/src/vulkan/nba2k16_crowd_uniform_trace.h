#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace renderer::vulkan::diagnostics {

inline constexpr size_t NBA2K16_CROWD_UNIFORM_BYTES = 1616;
inline constexpr size_t NBA2K16_CROWD_INSTANCE_OFFSET = 64;
inline constexpr size_t NBA2K16_CROWD_INSTANCE_BYTES = 400;
inline constexpr size_t NBA2K16_CROWD_MATRIX_OFFSET = 464;
inline constexpr size_t NBA2K16_CROWD_MATRIX_BYTES = 1152;
inline constexpr uint32_t NBA2K16_CROWD_UNIFORM_TRACE_LIMIT = 16;

struct Nba2k16CrowdUniformHashes {
    uint64_t full = 0;
    uint64_t instance = 0;
    uint64_t matrix = 0;
};

inline int nba2k16_crowd_hex_nibble(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

inline bool nba2k16_crowd_uniform_trace_enabled(const uint8_t *program_hash, size_t hash_size) {
    static constexpr uint8_t target_hash[] = {
        0x6e, 0xc1, 0xc3, 0x20, 0xe5, 0x85, 0xac, 0x48,
        0x6e, 0xb3, 0x7b, 0x91, 0x1d, 0xed, 0x7d, 0x88,
        0x42, 0xd0, 0xa0, 0x12, 0xaa, 0x98, 0x1a, 0xbd,
        0x2d, 0x3e, 0x8a, 0xd2, 0xe2, 0x24, 0xb5, 0x77,
    };
    const char *const title = std::getenv("VITA3K_TTBR_REMAP_HLE_TITLE");
    const char *const enabled = std::getenv("VITA3K_NBA2K16_CROWD_UNIFORM_TRACE");
    return title && std::strcmp(title, "NBA2K1601") == 0
        && enabled && enabled[0] == '1' && enabled[1] == 0
        && program_hash && hash_size == sizeof(target_hash)
        && std::memcmp(program_hash, target_hash, sizeof(target_hash)) == 0;
}

inline uint64_t nba2k16_crowd_uniform_hash(const uint8_t *bytes, size_t size) {
    if (!bytes && size)
        return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

inline Nba2k16CrowdUniformHashes nba2k16_crowd_uniform_hashes(const uint8_t *bytes, size_t size) {
    if (!bytes || size < NBA2K16_CROWD_UNIFORM_BYTES)
        return {};
    return {
        nba2k16_crowd_uniform_hash(bytes, NBA2K16_CROWD_UNIFORM_BYTES),
        nba2k16_crowd_uniform_hash(bytes + NBA2K16_CROWD_INSTANCE_OFFSET, NBA2K16_CROWD_INSTANCE_BYTES),
        nba2k16_crowd_uniform_hash(bytes + NBA2K16_CROWD_MATRIX_OFFSET, NBA2K16_CROWD_MATRIX_BYTES),
    };
}

} // namespace renderer::vulkan::diagnostics
