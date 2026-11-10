#include "mapping_policy.h"

#include <gtest/gtest.h>

namespace {
struct Range {
    uint32_t user_va;
    uint32_t guest_va;
    uint32_t size;
    uint32_t perm;
};
}

TEST(ttbr_remap_mapping_policy, accepts_overlapping_identity_ranges) {
    const Range existing{ 0x81000000u, 0x81000000u, 0x006A5000u, 7u };
    const Range requested{ 0x81000000u, 0x81000000u, 0x006A6000u, 7u };
    EXPECT_TRUE(ttbr_remap_overlapping_mapping_supported(existing, requested));
}

TEST(ttbr_remap_mapping_policy, rejects_overlap_with_an_alias) {
    const Range identity{ 0x81000000u, 0x81000000u, 0x2000u, 7u };
    const Range alias{ 0x82000000u, 0x81000000u, 0x2000u, 7u };
    EXPECT_FALSE(ttbr_remap_overlapping_mapping_supported(identity, alias));
    EXPECT_FALSE(ttbr_remap_overlapping_mapping_supported(alias, identity));
}

TEST(ttbr_remap_mapping_policy, rejects_permission_conflict) {
    const Range executable{ 0x81000000u, 0x81000000u, 0x2000u, 7u };
    const Range writable{ 0x81001000u, 0x81001000u, 0x2000u, 3u };
    EXPECT_FALSE(ttbr_remap_overlapping_mapping_supported(executable, writable));
}

TEST(ttbr_remap_mapping_policy, resumes_only_benign_quantized_stops) {
    EXPECT_TRUE(ttbr_remap_should_emit_timer_exit(true, 0, 2000, true));
    EXPECT_FALSE(ttbr_remap_should_emit_timer_exit(false, 0, 2000, true));
    EXPECT_FALSE(ttbr_remap_should_emit_timer_exit(true, 1, 2000, true));
    EXPECT_FALSE(ttbr_remap_should_emit_timer_exit(true, 0, 0, true));
    EXPECT_FALSE(ttbr_remap_should_emit_timer_exit(true, 0, 2000, false));
}
