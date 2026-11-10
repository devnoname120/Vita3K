#pragma once

#include <cstdint>

template <typename Range>
constexpr bool ttbr_remap_overlapping_mapping_supported(const Range &existing, const Range &requested) {
    if (!existing.size || !requested.size || existing.perm != requested.perm)
        return false;
    if (existing.user_va != existing.guest_va || requested.user_va != requested.guest_va)
        return false;

    const uint64_t existing_end = uint64_t(existing.guest_va) + existing.size;
    const uint64_t requested_end = uint64_t(requested.guest_va) + requested.size;
    return uint64_t(existing.guest_va) < requested_end &&
        uint64_t(requested.guest_va) < existing_end;
}

constexpr bool ttbr_remap_should_emit_timer_exit(bool run_error, int cpu_result,
    uint32_t quantum_us, bool thread_can_resume) {
    return run_error && cpu_result == 0 && quantum_us != 0 && thread_can_resume;
}
