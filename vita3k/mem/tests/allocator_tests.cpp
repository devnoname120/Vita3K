// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <cstring>
#include <list>
#include <mem/allocator.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <mem/util.h>

#include <gtest/gtest.h>
#include <mem/state.h>

TEST(guest_memory, fixed_low_pages_do_not_inherit_host_null_reservation) {
    MemState state;
    ASSERT_TRUE(init(state, true));
    ASSERT_FALSE(is_valid_addr(state, 0));
    ASSERT_EQ(try_alloc_at(state, 0x1000, 0x1000, "low-text"), 0x1000u);
    ASSERT_EQ(try_alloc_at(state, 0x2000, 0x1000, "low-data"), 0x2000u);
    state.memory[0x1000] = 0x31;
    state.memory[0x2000] = 0x52;
    ASSERT_EQ(try_alloc_at(state, 0x1000, 0x1000, "collision"), 0u);
    free(state, 0x1000);
    EXPECT_EQ(state.memory[0x2000], 0x52);
    ASSERT_EQ(try_alloc_at(state, 0x1000, 0x1000, "low-text-again"), 0x1000u);
    EXPECT_EQ(state.memory[0x1000], 0);
    EXPECT_EQ(state.memory[0x2000], 0x52);
    free(state, 0x1000);
    free(state, 0x2000);
}

TEST(guest_memory, alias_mapping_preserves_backing_and_stays_coherent) {
    MemState state;
    ASSERT_TRUE(init(state, true));
    const Address backing = alloc_at(state, 0x80000000, 0x2000, "alias-backing");
    ASSERT_EQ(backing, 0x80000000u);
    auto *backing_ptr = Ptr<uint8_t>(backing).get(state);
    backing_ptr[0] = 0x31;
    backing_ptr[0x1000] = 0x52;

    ASSERT_TRUE(add_alias_mapping(state, 0x4000, backing, 0x2000));
    auto *alias_ptr = Ptr<uint8_t>(0x4000).get(state);
    EXPECT_EQ(alias_ptr[0], 0x31);
    EXPECT_EQ(alias_ptr[0x1000], 0x52);
    alias_ptr[7] = 0xA4;
    EXPECT_EQ(backing_ptr[7], 0xA4);

    ASSERT_TRUE(remove_alias_mapping(state, 0x4000, 0x2000));
    EXPECT_FALSE(is_valid_addr(state, 0x4000));
    EXPECT_EQ(backing_ptr[0], 0x31);
    EXPECT_EQ(backing_ptr[7], 0xA4);
    free(state, backing);
    deinit_mem(state);
}

TEST(guest_memory, finds_backing_allocation_from_interior_address) {
    MemState state;
    ASSERT_TRUE(init(state, true));
    const Address allocation = alloc_at(state, 0x81000000, 0x2800, "allocation-range");
    ASSERT_EQ(allocation, 0x81000000u);

    Address base = 0;
    uint32_t size = 0;
    ASSERT_TRUE(find_backing_allocation_range(state, allocation + 0x1800, base, size));
    EXPECT_EQ(base, allocation);
    EXPECT_EQ(size, 0x3000u);
    EXPECT_FALSE(find_backing_allocation_range(state, allocation + 0x3000, base, size));

    free(state, allocation);
    EXPECT_FALSE(find_backing_allocation_range(state, allocation + 0x1800, base, size));
    deinit_mem(state);
}

TEST(guest_memory, excludes_aliases_from_backing_allocation_lookup) {
    MemState state;
    ASSERT_TRUE(init(state, true));
    const Address backing = alloc_at(state, 0x80000000, 0x2000, "alias-backing-range");
    ASSERT_EQ(backing, 0x80000000u);
    ASSERT_TRUE(add_alias_mapping(state, 0x4000, backing, 0x2000));

    Address base = 0;
    uint32_t size = 0;
    EXPECT_TRUE(find_backing_allocation_range(state, backing + 0x1000, base, size));
    EXPECT_EQ(base, backing);
    EXPECT_EQ(size, 0x2000u);
    EXPECT_FALSE(find_backing_allocation_range(state, 0x5000, base, size));

    ASSERT_TRUE(remove_alias_mapping(state, 0x4000, 0x2000));
    free(state, backing);
    deinit_mem(state);
}

TEST(guest_memory, limits_contiguous_span_at_translation_boundaries) {
    MemState state;
    ASSERT_TRUE(init(state, true));

    const Address regular = alloc_at(state, 0x81000000, 0x4000, "regular-span");
    ASSERT_EQ(regular, 0x81000000u);
    EXPECT_EQ(mapped_contiguous_size(state, regular + 0xFF0, 0x2020), 0x2020u);

    const Address first_backing = alloc_at(state, 0x80000000, 0x1000, "first-backing");
    const Address second_backing = alloc_at(state, 0x80002000, 0x1000, "second-backing");
    ASSERT_EQ(first_backing, 0x80000000u);
    ASSERT_EQ(second_backing, 0x80002000u);
    ASSERT_TRUE(add_alias_mapping(state, 0x4000, first_backing, 0x1000));
    ASSERT_TRUE(add_alias_mapping(state, 0x5000, second_backing, 0x1000));

    EXPECT_EQ(mapped_contiguous_size(state, 0x4FF0, 0x30), 0x10u);
    EXPECT_EQ(mapped_contiguous_size(state, 0x5000, 0x30), 0x30u);
    EXPECT_EQ(mapped_contiguous_size(state, 0x6000, 0x30), 0u);
    EXPECT_EQ(mapped_contiguous_size(state, 0xFFFFFFF0, 0x30), 0u);

    ASSERT_TRUE(remove_alias_mapping(state, 0x4000, 0x1000));
    ASSERT_TRUE(remove_alias_mapping(state, 0x5000, 0x1000));
    free(state, first_backing);
    free(state, second_backing);
    free(state, regular);
    deinit_mem(state);
}

TEST(guest_memory, copies_across_noncontiguous_page_translations) {
    MemState state;
    ASSERT_TRUE(init(state, true));

    const Address source_first = alloc_at(state, 0x80000000, 0x1000, "copy-source-first");
    const Address source_second = alloc_at(state, 0x80002000, 0x1000, "copy-source-second");
    const Address destination_first = alloc_at(state, 0x80004000, 0x1000, "copy-destination-first");
    const Address destination_second = alloc_at(state, 0x80006000, 0x1000, "copy-destination-second");
    ASSERT_EQ(source_first, 0x80000000u);
    ASSERT_EQ(source_second, 0x80002000u);
    ASSERT_EQ(destination_first, 0x80004000u);
    ASSERT_EQ(destination_second, 0x80006000u);

    ASSERT_TRUE(add_alias_mapping(state, 0x4000, source_first, 0x1000));
    ASSERT_TRUE(add_alias_mapping(state, 0x5000, source_second, 0x1000));
    ASSERT_TRUE(add_alias_mapping(state, 0x6000, destination_first, 0x1000));
    ASSERT_TRUE(add_alias_mapping(state, 0x7000, destination_second, 0x1000));

    auto *source_first_ptr = Ptr<uint8_t>(source_first).get(state);
    auto *source_second_ptr = Ptr<uint8_t>(source_second).get(state);
    auto *destination_first_ptr = Ptr<uint8_t>(destination_first).get(state);
    auto *destination_second_ptr = Ptr<uint8_t>(destination_second).get(state);
    for (uint32_t index = 0; index < 8; ++index)
        source_first_ptr[0xFF8 + index] = static_cast<uint8_t>(0x20 + index);
    for (uint32_t index = 0; index < 24; ++index)
        source_second_ptr[index] = static_cast<uint8_t>(0x28 + index);
    std::memset(destination_first_ptr + 0xFF8, 0xA5, 8);
    std::memset(destination_second_ptr, 0xA5, 24);

    ASSERT_TRUE(copy_guest_memory(state, 0x6FF8, 0x4FF8, 32));
    for (uint32_t index = 0; index < 8; ++index)
        EXPECT_EQ(destination_first_ptr[0xFF8 + index], static_cast<uint8_t>(0x20 + index));
    for (uint32_t index = 0; index < 24; ++index)
        EXPECT_EQ(destination_second_ptr[index], static_cast<uint8_t>(0x28 + index));

    std::memset(destination_first_ptr + 0xFF8, 0x5A, 8);
    std::memset(destination_second_ptr, 0x5A, 24);
    EXPECT_FALSE(copy_guest_memory(state, 0x6FF8, 0x8FF8, 32));
    for (uint32_t index = 0; index < 8; ++index)
        EXPECT_EQ(destination_first_ptr[0xFF8 + index], 0x5A);
    for (uint32_t index = 0; index < 24; ++index)
        EXPECT_EQ(destination_second_ptr[index], 0x5A);

    ASSERT_TRUE(remove_alias_mapping(state, 0x4000, 0x1000));
    ASSERT_TRUE(remove_alias_mapping(state, 0x5000, 0x1000));
    ASSERT_TRUE(remove_alias_mapping(state, 0x6000, 0x1000));
    ASSERT_TRUE(remove_alias_mapping(state, 0x7000, 0x1000));
    free(state, source_first);
    free(state, source_second);
    free(state, destination_first);
    free(state, destination_second);
    deinit_mem(state);
}

TEST(bitmap_allocator, one_bit_allocation) {
    BitmapAllocator allocator(KiB(5));

    for (int i = 0; i < KiB(5); ++i) {
        uint32_t size = 1;
        int inoffset = 31 - i & 31;
        int bit = (allocator.words[i >> 5] & (1 << inoffset)) >> inoffset;
        ASSERT_EQ(bit, 1);
        int ret = allocator.allocate_from(i, size, false);
        ASSERT_EQ(ret, i);
        bit = (allocator.words[i >> 5] & (1 << inoffset)) >> inoffset;
        ASSERT_EQ(bit, 0);
    }
}

TEST(bitmap_allocator, one_bit_free_slot) {
    BitmapAllocator allocator(KiB(5));

    ASSERT_EQ(allocator.free_slot_count(0, KiB(5)), KiB(5));
    for (int i = 0; i < KiB(5); ++i) {
        uint32_t size = 1;
        ASSERT_EQ(allocator.free_slot_count(i, i + 1), 1);
        int ret = allocator.allocate_from(i, size, false);
        ASSERT_EQ(ret, i);
        ASSERT_EQ(size, 1);
        ASSERT_EQ(allocator.free_slot_count(i, i + 1), 0);
    }
    ASSERT_EQ(allocator.free_slot_count(0, KiB(5)), 0);
}

TEST(bitmap_allocator, free_slot_count_aligned) {
    BitmapAllocator allocator(KiB(5));

    for (int i = 0; i < KiB(5); i += 2) {
        uint32_t size = 2;
        ASSERT_EQ(allocator.free_slot_count(i, i + 2), 2);
        int ret = allocator.allocate_from(i, size, false);
        ASSERT_EQ(ret, i);
        ASSERT_EQ(size, 2);
        ASSERT_EQ(allocator.free_slot_count(i, i + 2), 0);
    }
}

TEST(bitmap_allocator, free_slot_count_unaligned) {
    BitmapAllocator allocator(KiB(5));

    for (int i = 0; KiB(5) - i >= 3; i += 3) {
        uint32_t size = 3;
        ASSERT_EQ(allocator.free_slot_count(i, i + 3), 3);
        int ret = allocator.allocate_from(i, size, false);
        ASSERT_EQ(ret, i);
        ASSERT_EQ(size, 3);
        ASSERT_EQ(allocator.free_slot_count(i, i + 3), 0);
    }
}

TEST(bitmap_allocator, allocate_at) {
    BitmapAllocator allocator(KiB(5));

    ASSERT_EQ(allocator.free_slot_count(0, KiB(5)), KiB(5));
    for (int i = 0; KiB(5) - i >= 4; i += 4) {
        uint32_t size = 4;
        ASSERT_EQ(allocator.free_slot_count(i, i + 4), 4);
        allocator.allocate_at(i, 4);
        ASSERT_EQ(allocator.free_slot_count(i, i + 4), 0);
        allocator.free(i, size);
        ASSERT_EQ(size, 4);
        ASSERT_EQ(allocator.free_slot_count(i, i + 4), 4);
    }
    ASSERT_EQ(allocator.free_slot_count(0, KiB(5)), KiB(5));
}

TEST(bitmap_allocator, alloc_32) {
    BitmapAllocator allocator(KiB(5));
    for (int i = 0; i < KiB(5); i += 32) {
        uint32_t size = 32;
        ASSERT_EQ(allocator.free_slot_count(i, i + 32), 32);
        int ret = allocator.allocate_from(i, size, false);
        ASSERT_EQ(ret, i);
        ASSERT_EQ(size, 32);
        ASSERT_EQ(allocator.free_slot_count(i, i + 32), 0);
    }
}

TEST(bitmap_allocator, battle_test) {
    srand(time(0));
    constexpr int MEM_SIZE = KiB(10);
    constexpr int TEST_EPOCH = KiB(1000);
    constexpr int MAX_MEM_CHUNK_SIZE = 100;

    BitmapAllocator allocator(MEM_SIZE);
    struct Page {
        int n;
        uint32_t size;
    };

    std::list<Page> pages;

    int tracked_size = MEM_SIZE;
    const auto free_any_page = [&]() {
        if (!pages.empty()) {
            auto it = pages.begin();
            allocator.free(it->n, it->size);
            tracked_size += it->size;
            pages.erase(it);
        }
    };

    for (int i = 0; i < TEST_EPOCH; ++i) {
        uint32_t size = rand() % MAX_MEM_CHUNK_SIZE + 1;
        while (allocator.free_slot_count(0, MEM_SIZE) < size) {
            free_any_page();
        }
        ASSERT_EQ(allocator.free_slot_count(0, MEM_SIZE), tracked_size);
        int offset = rand() % MEM_SIZE;
        int ret = allocator.allocate_at(offset, size);
        if (ret < 0) {
            offset = allocator.allocate_from(0, size, rand() % 2);
        }
        if (offset >= 0) {
            tracked_size -= size;
            ASSERT_EQ(allocator.free_slot_count(offset, offset + size), 0);
            ASSERT_EQ(allocator.free_slot_count(0, MEM_SIZE), tracked_size);
            pages.push_back({ offset, size });
        } else {
            free_any_page();
        }
    }
}

// These tests are from EKA2L1 (https://github.com/EKA2L1/EKA2L1/blob/4fbd057da2a0c4f66a5c0f9dfc406c5d90f7531f/src/tests/common/allocator.cpp)
TEST(bitmap_allocator, no_best_fit_only_one_fit) {
    BitmapAllocator alloc(32);

    // Bitmap:              1000 0101 0001 0001 0101 0001 00[11 1]001
    alloc.words[0] = 0b10000101000100010101000100111001;

    uint32_t to_alloc = 3;
    ASSERT_EQ(alloc.allocate_from(0, to_alloc), 26);
    ASSERT_EQ(to_alloc, 3);

    // After allocate:      1000 0101 0001 0001 0101 0001 00[00 0]001
    ASSERT_EQ(alloc.words[0], 0b10000101000100010101000100000001);
}

TEST(bitmap_allocator, no_best_fit_multiple_fit) {
    BitmapAllocator alloc(32);

    // Bitmap 1:            1000 0[111] 1001 0001 0101 0001 0011 1001
    alloc.words[0] = 0b10000111100100010101000100111001;

    uint32_t to_alloc = 3;
    ASSERT_EQ(alloc.allocate_from(0, to_alloc), 5);
    ASSERT_EQ(to_alloc, 3);

    // After allocate:      1000 0[000] 1001 0001 0101 0001 0011 1001
    ASSERT_EQ(alloc.words[0], 0b10000000100100010101000100111001);
}

TEST(bitmap_allocator, no_best_fit_alloc_across) {
    BitmapAllocator alloc(64);

    alloc.words[0] = 0b111;
    alloc.words[1] = 0b11100000000000000000000000000000;

    uint32_t to_alloc = 5;
    ASSERT_EQ(alloc.allocate_from(0, to_alloc), 29);
    ASSERT_EQ(to_alloc, 5);

    ASSERT_EQ(alloc.words[0], 0);
    ASSERT_EQ(alloc.words[1], 0b00100000000000000000000000000000);
}

TEST(bitmap_allocator, best_fit_multiple_fit) {
    BitmapAllocator alloc(32);

    // Bitmap 1:            1000 0111 1001 0001 0101 0001 00[11 1]001
    alloc.words[0] = 0b10000111100100010101000100111001;

    uint32_t to_alloc = 3;
    ASSERT_EQ(alloc.allocate_from(0, to_alloc, true), 26);
    ASSERT_EQ(to_alloc, 3);

    // After allocate:      1000 0111 1001 0001 0101 0001 00[00 0]001
    ASSERT_EQ(alloc.words[0], 0b10000111100100010101000100000001);
}

TEST(bitmap_allocator, count_bit_aligned) {
    BitmapAllocator alloc(32 * 3);

    // Bitmap 1: All bit on
    alloc.words[0] = 0xFFFFFFFF;

    // Bitmap 2: 12 bits on
    alloc.words[1] = 0xF00F00F0;

    // Bitmap 3: 12 bits on
    alloc.words[2] = 0x00F00F0F;

    ASSERT_EQ(alloc.free_slot_count(0, 32 * 3), 56);
    ASSERT_EQ(alloc.free_slot_count(0, 32 * 3 - 1), 55);
}

TEST(bitmap_allocator, count_unaligned) {
    BitmapAllocator alloc(32 * 3);

    // Bitmap 1: 0b11 | 0011000011
    //      ignored ^
    // 4 valid bits on
    alloc.words[0] = 0b110011000011;

    // Bitmap 2: 12 bits on
    alloc.words[1] = 0xF00F00F0;

    // Bitmap 3: 0b1010111000 | 1111
    //                          ^ ignored
    // 5 valid bits on
    alloc.words[2] = 0b10101110001111;

    // 4 valid bits + 12 bits + 5 valid bits = 21
    ASSERT_EQ(alloc.free_slot_count(22, 92), 21);
}
