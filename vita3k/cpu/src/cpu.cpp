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

#include <cpu/disasm/functions.h>
#include <cpu/functions.h>
#include <cpu/impl/dynarmic_cpu.h>
#include <cpu/impl/interface.h>
#include <cpu/state.h>
#include <mem/ptr.h>
#include <util/types.h>

#include <memory>
#include <string>

static void delete_cpu_state(CPUState *state) {
    delete state;
}

void set_thread_id(CPUState &state, SceUID thread_id) {
    state.thread_id = thread_id;
}

SceUID get_thread_id(CPUState &state) {
    return state.thread_id;
}

CPUStatePtr init_cpu(bool cpu_opt, SceUID thread_id, std::size_t processor_id, MemState &mem) {
    CPUStatePtr state(new CPUState(), delete_cpu_state);
    state->mem = &mem;
    state->thread_id = thread_id;

    if (!init(state->disasm)) {
        return CPUStatePtr();
    }

    state->cpu = std::make_unique<DynarmicCPU>(state.get(), processor_id, cpu_opt);

    return state;
}

int run(CPUState &state) {
    return state.cpu->run();
}

int step(CPUState &state) {
    return state.cpu->step();
}

void stop(CPUState &state) {
    state.cpu->stop();
}

uint32_t read_reg(CPUState &state, size_t index) {
    return state.cpu->get_reg(index);
}

float read_float_reg(CPUState &state, size_t index) {
    return state.cpu->get_float_reg(index);
}

void write_float_reg(CPUState &state, size_t index, float value) {
    state.cpu->set_float_reg(index, value);
}

void write_fpscr(CPUState &state, uint32_t value) {
    state.cpu->set_fpscr(value);
}

void write_cpsr(CPUState &state, uint32_t value) {
    state.cpu->set_cpsr(value);
}

uint32_t read_sp(CPUState &state) {
    return state.cpu->get_sp();
}

uint32_t read_pc(CPUState &state) {
    return state.cpu->get_pc();
}

uint32_t read_lr(CPUState &state) {
    return state.cpu->get_lr();
}

uint32_t read_fpscr(CPUState &state) {
    return state.cpu->get_fpscr();
}

uint32_t read_cpsr(CPUState &state) {
    return state.cpu->get_cpsr();
}

uint32_t read_tpidruro(CPUState &state) {
    return state.cpu->get_tpidruro();
}

uint32_t read_tpidrurw(CPUState &state) {
    return state.cpu->get_tpidrurw();
}

void write_reg(CPUState &state, size_t index, uint32_t value) {
    state.cpu->set_reg(index, value);
}

void write_sp(CPUState &state, uint32_t value) {
    state.cpu->set_sp(value);
}

void write_pc(CPUState &state, uint32_t value) {
    state.cpu->set_pc(value);
}

void write_lr(CPUState &state, uint32_t value) {
    state.cpu->set_lr(value);
}

void write_tpidruro(CPUState &state, uint32_t value) {
    state.cpu->set_tpidruro(value);
}

void write_tpidrurw(CPUState &state, uint32_t value) {
    state.cpu->set_tpidrurw(value);
}

bool is_thumb_mode(CPUState &state) {
    return state.cpu->is_thumb_mode();
}

bool hit_breakpoint(CPUState &state) {
    return state.cpu->hit_breakpoint();
}

void trigger_breakpoint(CPUState &state) {
    state.cpu->trigger_breakpoint();
}

void set_log_code(CPUState &state, bool log) {
    state.cpu->set_log_code(log);
}

void set_log_mem(CPUState &state, bool log) {
    state.cpu->set_log_mem(log);
}

bool get_log_code(CPUState &state) {
    return state.cpu->get_log_code();
}

bool get_log_mem(CPUState &state) {
    return state.cpu->get_log_mem();
}

void clear_exclusive(CPUState &state) {
    state.cpu->clear_exclusive();
}

std::size_t get_processor_id(CPUState &state) {
    return state.cpu->processor_id();
}

void invalidate_jit_cache(CPUState &state, Address start, size_t length) {
    state.cpu->invalidate_jit_cache(start, length);
}

std::string disassemble(CPUState &state, uint64_t at, bool thumb, uint16_t *insn_size) {
    MemState &mem = *state.mem;
    const uint8_t *const code = Ptr<const uint8_t>(static_cast<Address>(at)).get(mem);
    const size_t buffer_size = GiB(4) - at;
    return disassemble(state.disasm, code, buffer_size, at, thumb, insn_size);
}

std::string disassemble(CPUState &state, uint64_t at, uint16_t *insn_size) {
    const bool thumb = state.cpu->is_thumb_mode();
    return disassemble(state, at, thumb, insn_size);
}

CPUContext save_context(CPUState &state) {
    return state.cpu->save_context();
}

void load_context(CPUState &state, const CPUContext &ctx) {
    state.cpu->load_context(ctx);
}

CPUContextRunResult run_context_until_trap(CPUState &state, const CPUContext &context,
    uint32_t tpidruro, uint32_t tpidrurw) {
    return run_context_until_trap(state, context, tpidruro, tpidrurw,
        CPU_RUN_UNBOUNDED_TICKS);
}

CPUContextRunResult run_context_until_trap(CPUState &state, const CPUContext &context,
    uint32_t tpidruro, uint32_t tpidrurw, uint64_t tick_budget) {
    const CPUContext caller_context = save_context(state);
    const uint32_t caller_tpidruro = read_tpidruro(state);
    const uint32_t caller_tpidrurw = read_tpidrurw(state);
    const bool caller_svc_called = state.svc_called;
    const uint32_t caller_svc = state.svc;
    const bool caller_abort_pending = state.abort_pending.exchange(false);
    const uint32_t caller_abort_fault_addr = state.abort_fault_addr.load();
    const bool caller_abort_is_write = state.abort_is_write.load();
    const uint64_t caller_ticks_remaining = state.ticks_remaining;

    load_context(state, context);
    write_tpidruro(state, tpidruro);
    write_tpidrurw(state, tpidrurw);
    state.svc_called = false;
    state.svc = 0;
    state.abort_fault_addr.store(0);
    state.abort_is_write.store(false);
    state.ticks_remaining = tick_budget;

    CPUContextRunResult result;
    result.cpu_result = run(state);
    result.context = save_context(state);
    result.tpidruro = read_tpidruro(state);
    result.tpidrurw = read_tpidrurw(state);
    result.svc = state.svc;
    result.fault_address = state.abort_fault_addr.load();
    result.fault_is_write = state.abort_is_write.load();

    if (state.abort_pending.exchange(false)) {
        result.reason = CPUContextRunReason::Abort;
    } else if (state.svc_called) {
        result.reason = CPUContextRunReason::Svc;
    } else if (result.cpu_result > 0) {
        result.reason = CPUContextRunReason::Halt;
    } else {
        result.reason = CPUContextRunReason::Error;
    }

    load_context(state, caller_context);
    write_tpidruro(state, caller_tpidruro);
    write_tpidrurw(state, caller_tpidrurw);
    state.svc_called = caller_svc_called;
    state.svc = caller_svc;
    state.abort_fault_addr.store(caller_abort_fault_addr);
    state.abort_is_write.store(caller_abort_is_write);
    state.abort_pending.store(caller_abort_pending);
    state.ticks_remaining = caller_ticks_remaining;
    return result;
}

uint32_t stack_alloc(CPUState &state, size_t size) {
    const uint32_t new_sp = read_sp(state) - size;
    write_sp(state, new_sp);
    return new_sp;
}

uint32_t stack_free(CPUState &state, size_t size) {
    const uint32_t new_sp = read_sp(state) + size;
    write_sp(state, new_sp);
    return new_sp;
}

static thread_local CPUState *current_cpu_state = nullptr;

void set_current_cpu_state(CPUState *state) {
    current_cpu_state = state;
}

CPUState *get_current_cpu_state() {
    return current_cpu_state;
}
