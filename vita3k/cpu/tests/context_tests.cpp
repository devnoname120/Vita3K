// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <cpu/functions.h>
#include <cpu/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>

#include <gtest/gtest.h>

TEST(cpu_context, nested_run_stops_on_svc_and_restores_caller) {
    MemState mem;
    ASSERT_TRUE(init(mem, true));
    ASSERT_EQ(try_alloc_at(mem, 0x4000, 0x2000, "nested-context-test"), 0x4000u);
    *Ptr<uint32_t>(0x4000).get(mem) = 0xEF000123u;
    *Ptr<uint32_t>(0x5000).get(mem) = 0xEF00DEADu;

    auto cpu = init_cpu(false, 7, 0, mem);
    ASSERT_NE(cpu, nullptr);

    CPUContext caller;
    caller.cpu_registers[0] = 0x11111111u;
    caller.cpu_registers[13] = 0x8000u;
    caller.cpu_registers[15] = 0x5000u;
    caller.cpsr = 0x10u;
    load_context(*cpu, caller);
    write_tpidruro(*cpu, 0xAAAA0001u);
    write_tpidrurw(*cpu, 0xBBBB0002u);
    ASSERT_EQ(run(*cpu), 0);
    ASSERT_TRUE(cpu->svc_called);
    ASSERT_EQ(cpu->svc, 0xDEADu);
    caller.cpu_registers[15] = 0x5004u;

    CPUContext guest;
    guest.cpu_registers[0] = 0x12345678u;
    guest.cpu_registers[13] = 0x7000u;
    guest.cpu_registers[15] = 0x4000u;
    guest.cpsr = 0x10u;

    const auto result = run_context_until_trap(
        *cpu, guest, 0xCCCC0003u, 0xDDDD0004u);

    EXPECT_EQ(result.reason, CPUContextRunReason::Svc);
    EXPECT_EQ(result.svc, 0x123u);
    EXPECT_EQ(result.context.cpu_registers[0], 0x12345678u);
    EXPECT_EQ(result.context.cpu_registers[15], 0x4004u);
    EXPECT_EQ(result.tpidruro, 0xCCCC0003u);
    EXPECT_EQ(result.tpidrurw, 0xDDDD0004u);

    const CPUContext restored = save_context(*cpu);
    EXPECT_EQ(restored.cpu_registers, caller.cpu_registers);
    EXPECT_EQ(restored.cpsr, caller.cpsr);
    EXPECT_EQ(read_tpidruro(*cpu), 0xAAAA0001u);
    EXPECT_EQ(read_tpidrurw(*cpu), 0xBBBB0002u);
    EXPECT_TRUE(cpu->svc_called);
    EXPECT_EQ(cpu->svc, 0xDEADu);

    free(mem, 0x4000);
    deinit_mem(mem);
}

TEST(cpu_context, nested_run_stops_at_tick_budget_and_restores_caller) {
    MemState mem;
    ASSERT_TRUE(init(mem, true));
    ASSERT_EQ(try_alloc_at(mem, 0x4000, 0x1000, "nested-budget-test"), 0x4000u);
    *Ptr<uint32_t>(0x4000).get(mem) = 0xEAFFFFFEu;

    auto cpu = init_cpu(false, 7, 0, mem);
    ASSERT_NE(cpu, nullptr);

    CPUContext caller;
    caller.cpu_registers[0] = 0x11111111u;
    caller.cpu_registers[13] = 0x8000u;
    caller.cpu_registers[15] = 0x5000u;
    caller.cpsr = 0x10u;
    load_context(*cpu, caller);
    cpu->ticks_remaining = 0x12345678u;

    CPUContext guest;
    guest.cpu_registers[13] = 0x7000u;
    guest.cpu_registers[15] = 0x4000u;
    guest.cpsr = 0x10u;

    const auto result = run_context_until_trap(*cpu, guest, 0, 0, 64u);

    EXPECT_EQ(result.reason, CPUContextRunReason::Error);
    EXPECT_EQ(result.cpu_result, 0);
    EXPECT_EQ(result.context.cpu_registers[15], 0x4000u);
    EXPECT_EQ(save_context(*cpu).cpu_registers, caller.cpu_registers);
    EXPECT_EQ(cpu->ticks_remaining, 0x12345678u);

    free(mem, 0x4000);
    deinit_mem(mem);
}

TEST(cpu_context, svc_preserves_softfp_arguments_in_vfp_registers) {
    MemState mem;
    ASSERT_TRUE(init(mem, true));
    ASSERT_EQ(try_alloc_at(mem, 0x4000, 0x2000, "softfp-svc-test"), 0x4000u);

    static constexpr uint16_t wrapper[] = {
        0xEE00u, 0x1A10u,
        0xEE00u, 0x2A90u,
        0xEE01u, 0x3A10u,
        0x9900u,
        0xEE01u, 0x1A90u,
        0x9A01u,
        0xEE02u, 0x2A10u,
        0x9B02u,
        0xEE02u, 0x3A90u,
        0xE06Fu,
    };
    static constexpr uint16_t thumb_thunk[] = {
        0x4778u,
        0xE7FDu,
    };
    static constexpr uint32_t arm_thunk[] = {
        0xE59FC000u,
        0xE08CF00Fu,
        0x000000F0u,
    };
    static constexpr uint32_t import_stub[] = {
        0xEF000023u,
        0xEAFFFFFEu,
    };
    std::memcpy(Ptr<uint16_t>(0x4000).get(mem), wrapper, sizeof(wrapper));
    std::memcpy(Ptr<uint16_t>(0x4100).get(mem), thumb_thunk, sizeof(thumb_thunk));
    std::memcpy(Ptr<uint32_t>(0x4104).get(mem), arm_thunk, sizeof(arm_thunk));
    std::memcpy(Ptr<uint32_t>(0x4200).get(mem), import_stub, sizeof(import_stub));
    *Ptr<uint32_t>(0x5000).get(mem) = 0xC3700000u;
    *Ptr<uint32_t>(0x5004).get(mem) = 0x3F000000u;
    *Ptr<uint32_t>(0x5008).get(mem) = 0x3F000000u;

    auto cpu = init_cpu(false, 7, 0, mem);
    ASSERT_NE(cpu, nullptr);

    CPUContext context;
    context.cpu_registers[1] = 0x43070000u;
    context.cpu_registers[2] = 0x43070000u;
    context.cpu_registers[3] = 0x43700000u;
    context.cpu_registers[13] = 0x5000u;
    context.cpu_registers[15] = 0x4000u;
    context.cpsr = 0x30u;
    load_context(*cpu, context);

    ASSERT_EQ(run(*cpu), 0);
    ASSERT_TRUE(cpu->svc_called);
    ASSERT_EQ(cpu->svc, 0x23u);
    EXPECT_FLOAT_EQ(read_float_reg(*cpu, 0), 135.0f);
    EXPECT_FLOAT_EQ(read_float_reg(*cpu, 1), 135.0f);
    EXPECT_FLOAT_EQ(read_float_reg(*cpu, 2), 240.0f);
    EXPECT_FLOAT_EQ(read_float_reg(*cpu, 3), -240.0f);
    EXPECT_FLOAT_EQ(read_float_reg(*cpu, 4), 0.5f);
    EXPECT_FLOAT_EQ(read_float_reg(*cpu, 5), 0.5f);

    free(mem, 0x4000);
    deinit_mem(mem);
}
