// Vita3K emulator project
// Copyright (C) 2026 Vita3K team

#include <io/functions.h>
#include <io/io.h>
#include <io/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <mem/state.h>

#include <gtest/gtest.h>

#include <array>
#include <fstream>

namespace {
struct GuestIoFixture {
    MemState mem;
    Address first_backing = 0;
    Address second_backing = 0;

    GuestIoFixture() {
        EXPECT_TRUE(init(mem, true));
        first_backing = alloc_at(mem, 0x80000000, 0x1000, "guest-io-first");
        second_backing = alloc_at(mem, 0x80002000, 0x1000, "guest-io-second");
        EXPECT_EQ(first_backing, 0x80000000u);
        EXPECT_EQ(second_backing, 0x80002000u);
        EXPECT_TRUE(add_alias_mapping(mem, 0x4000, first_backing, 0x1000));
        EXPECT_TRUE(add_alias_mapping(mem, 0x5000, second_backing, 0x1000));
    }

    ~GuestIoFixture() {
        remove_alias_mapping(mem, 0x4000, 0x1000);
        remove_alias_mapping(mem, 0x5000, 0x1000);
        free(mem, first_backing);
        free(mem, second_backing);
        deinit_mem(mem);
    }
};
}

TEST(guest_io, reads_across_noncontiguous_alias_pages) {
    GuestIoFixture fixture;
    const fs::path path = fs::temp_directory_path() / fs::unique_path("vita3k-guest-read-%%%%-%%%%");
    std::array<uint8_t, 0x30> source{};
    for (uint32_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<uint8_t>(index + 1);
    {
        std::ofstream output(path.string(), std::ios::binary);
        ASSERT_TRUE(output.write(reinterpret_cast<const char *>(source.data()), source.size()));
    }

    IOState io{};
    io.std_files.emplace(3, FileStats("ux0:test.bin", "ux0:/test.bin", path, SCE_O_RDONLY));
    ASSERT_EQ(read_guest_file(Ptr<void>(0x4FF0), fixture.mem, io, 3, source.size(), "guest_io_test"),
        static_cast<int>(source.size()));

    const auto *first = Ptr<const uint8_t>(fixture.first_backing).get(fixture.mem);
    const auto *second = Ptr<const uint8_t>(fixture.second_backing).get(fixture.mem);
    EXPECT_TRUE(std::equal(source.begin(), source.begin() + 0x10, first + 0xFF0));
    EXPECT_TRUE(std::equal(source.begin() + 0x10, source.end(), second));

    io.std_files.clear();
    fs::remove(path);
}

TEST(guest_io, rejects_an_unmapped_tail_before_reading) {
    GuestIoFixture fixture;
    const fs::path path = fs::temp_directory_path() / fs::unique_path("vita3k-guest-invalid-read-%%%%-%%%%");
    std::array<uint8_t, 0x30> source{};
    for (uint32_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<uint8_t>(index + 1);
    {
        std::ofstream output(path.string(), std::ios::binary);
        ASSERT_TRUE(output.write(reinterpret_cast<const char *>(source.data()), source.size()));
    }

    IOState io{};
    io.std_files.emplace(3, FileStats("ux0:test.bin", "ux0:/test.bin", path, SCE_O_RDONLY));
    EXPECT_EQ(read_guest_file(Ptr<void>(0x5FF0), fixture.mem, io, 3, source.size(), "guest_io_test"),
        SCE_ERROR_ERRNO_EFAULT);
    EXPECT_EQ(io.std_files.at(3).tell(), 0);

    io.std_files.clear();
    fs::remove(path);
}

TEST(guest_io, writes_across_noncontiguous_alias_pages) {
    GuestIoFixture fixture;
    const fs::path path = fs::temp_directory_path() / fs::unique_path("vita3k-guest-write-%%%%-%%%%");
    std::array<uint8_t, 0x30> source{};
    for (uint32_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<uint8_t>(0x80 + index);
    auto *first = Ptr<uint8_t>(fixture.first_backing).get(fixture.mem);
    auto *second = Ptr<uint8_t>(fixture.second_backing).get(fixture.mem);
    std::copy(source.begin(), source.begin() + 0x10, first + 0xFF0);
    std::copy(source.begin() + 0x10, source.end(), second);
    {
        std::ofstream output(path.string(), std::ios::binary);
        ASSERT_TRUE(output.good());
    }

    IOState io{};
    io.std_files.emplace(4, FileStats("ux0:test.bin", "ux0:/test.bin", path,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC));
    ASSERT_EQ(write_guest_file(4, Ptr<const void>(0x4FF0), fixture.mem, source.size(), io, "guest_io_test"),
        static_cast<int>(source.size()));
    io.std_files.clear();

    std::array<uint8_t, 0x30> actual{};
    std::ifstream input(path.string(), std::ios::binary);
    ASSERT_TRUE(input.read(reinterpret_cast<char *>(actual.data()), actual.size()));
    EXPECT_EQ(actual, source);
    fs::remove(path);
}
