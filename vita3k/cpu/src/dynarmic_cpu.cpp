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

#include "cpu/common.h"
#include <cpu/impl/dynarmic_cpu.h>
#include <cpu/state.h>
#include <util/log.h>

#include <mem/ptr.h>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

static bool dynarmic_jit_lifetime_trace_enabled() {
    static const bool enabled = [] {
        const char *const value = std::getenv("VITA3K_DYNARMIC_JIT_TRACE");
        return value && value[0] == '1' && value[1] == 0;
    }();
    return enabled;
}

static std::size_t dynarmic_host_thread_tag() {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

static bool acxi_text_trace_enabled() {
    static const bool enabled = [] {
        const char *const value = std::getenv("VITA3K_ACXI_TEXT_TRACE");
        return value && value[0] == '1' && value[1] == 0;
    }();
    return enabled;
}

static bool acxi_menu_exec_trace_enabled() {
    static const bool enabled = [] {
        const char *const value = std::getenv("VITA3K_ACXI_EXEC_TRACE");
        return value && value[0] == '1' && value[1] == 0;
    }();
    return enabled;
}

static bool acxi_text_address(uint32_t address) {
    return address >= 0x1000u && address < 0xFA000u;
}

class ArmDynarmicCP15 : public Dynarmic::A32::Coprocessor {
    uint32_t tpidruro;
    uint32_t tpidrurw;
    uint32_t sctlr;
    uint32_t dacr;

public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    explicit ArmDynarmicCP15()
        : tpidruro(0)
        , tpidrurw(0)
        , sctlr(0)
        , dacr(0) {
    }

    ~ArmDynarmicCP15() override = default;

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, CoprocReg CRd,
        CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        return std::nullopt;
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn,
        CoprocReg CRm, unsigned opc2) override {
        // MCR p15, 0, Rt, c13, c0, 3 — write TPIDRURO
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro;
        }

        // iOS user TLS uses TPIDRURW in addition to Vita's TPIDRURO.
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 2) {
            return &tpidrurw;
        }

        // MCR p15, 0, Rt, c1, c0, 0 — write SCTLR
        if (!two && CRn == CoprocReg::C1 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &sctlr;
        }

        // MCR p15, 0, Rt, c3, c0, 0 — write DACR
        if (!two && CRn == CoprocReg::C3 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &dacr;
        }

        LOG_WARN("Unhandled CP15 MCR: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, (int)CRn, (int)CRm, opc2);
        return CallbackOrAccessOneWord{};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return CallbackOrAccessTwoWords{};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        // MRC p15, 0, Rt, c13, c0, 3 — read TPIDRURO (thread-local storage)
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro;
        }

        // iOS user TLS uses TPIDRURW in addition to Vita's TPIDRURO.
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 2) {
            return &tpidrurw;
        }

        // MRC p15, 0, Rt, c1, c0, 0 — read SCTLR
        if (!two && CRn == CoprocReg::C1 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &sctlr;
        }

        // MRC p15, 0, Rt, c3, c0, 0 — read DACR
        if (!two && CRn == CoprocReg::C3 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &dacr;
        }

        LOG_WARN("Unhandled CP15 MRC: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, (int)CRn, (int)CRm, opc2);
        return CallbackOrAccessOneWord{};
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return CallbackOrAccessTwoWords{};
    }

    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return std::nullopt;
    }

    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return std::nullopt;
    }

    void set_tpidruro(uint32_t tpidruro) {
        this->tpidruro = tpidruro;
    }

    uint32_t get_tpidruro() const {
        return tpidruro;
    }

    void set_tpidrurw(uint32_t tpidrurw) {
        this->tpidrurw = tpidrurw;
    }

    uint32_t get_tpidrurw() const {
        return tpidrurw;
    }
};

class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {
    friend class DynarmicCPU;

    CPUState *parent;
    DynarmicCPU *cpu;

public:
    explicit ArmDynarmicCallback(CPUState &parent, DynarmicCPU &cpu)
        : parent(&parent)
        , cpu(&cpu) {}

    ~ArmDynarmicCallback() override = default;

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr addr) override {
        if (cpu->log_mem)
            LOG_TRACE("Instruction fetch at address 0x{:X}", addr);
        return MemoryRead32(addr);
    }

    static void TraceInstruction(uint64_t self_, uint64_t address, uint64_t is_thumb) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);

        std::string disassembly = [&]() -> std::string {
            if (!address || !Ptr<uint32_t>{ (uint32_t)address }.valid(*self.parent->mem)) {
                return "invalid address";
            }
            return disassemble(*self.parent, address);
        }();
        LOG_TRACE("{} ({}): {} {}", log_hex(self_), self.parent->thread_id, log_hex(address), disassembly);
    }

    static uint32_t ReadAceWord(ArmDynarmicCallback &self, uint32_t address) {
        const Ptr<uint32_t> ptr{ address };
        return ptr && ptr.valid(*self.parent->mem) ? *ptr.get(*self.parent->mem) : 0;
    }

    static void TraceAceMenuExec(uint64_t self_, uint64_t address, uint64_t) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);
        const uint32_t current = ReadAceWord(self, 0x00112F60u);
        const uint32_t next = ReadAceWord(self, 0x00112F68u);
        if (current != 12u && next != 12u)
            return;

        const uint32_t pc = static_cast<uint32_t>(address);
        const uint32_t right = ReadAceWord(self, 0x0011303Cu);
        const uint32_t r0 = self.cpu->get_reg(0);
        unsigned slot = 0;
        switch (pc) {
        case 0x0008BB2Cu:
            slot = 0;
            break;
        case 0x0007A074u:
            slot = 1;
            break;
        case 0x00084F48u:
            if (r0 != right)
                return;
            slot = 2;
            break;
        case 0x00079268u:
            if (!right || r0 != right + 8u)
                return;
            slot = 3;
            break;
        case 0x0009F930u:
            slot = 4;
            break;
        case 0x0009EC4Cu:
            slot = 5;
            break;
        default:
            return;
        }

        static std::array<std::atomic<uint32_t>, 6> counts{};
        const uint32_t count = counts[slot].fetch_add(1, std::memory_order_relaxed) + 1u;
        if (count > 4u && (count & (count - 1u)) != 0u)
            return;

        const uint32_t task = ReadAceWord(self, 0x00112F5Cu);
        LOG_INFO("ACXI_EXEC_TRACE pc={} count={} thread={} r0={} current={} next={} state={} right={} rb_start={} rb_end={} rb_frame={} rb_play={} rb_flags={} rb_total={}",
            log_hex(pc), count, self.parent->thread_id, log_hex(r0), current, next,
            task ? ReadAceWord(self, task + 108u) : 0u, log_hex(right),
            right ? ReadAceWord(self, right + 36u) : 0u,
            right ? ReadAceWord(self, right + 40u) : 0u,
            right ? ReadAceWord(self, right + 44u) : 0u,
            right ? ReadAceWord(self, right + 48u) : 0u,
            right ? log_hex(ReadAceWord(self, right + 52u)) : log_hex(0u),
            right ? ReadAceWord(self, right + 60u) : 0u);
    }

    void PreCodeTranslationHook(bool is_thumb, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (cpu->log_code) {
            ir.CallHostFunction(&TraceInstruction, ir.Imm64((uint64_t)this), ir.Imm64(pc), ir.Imm64(is_thumb));
        }
        if (acxi_menu_exec_trace_enabled()) {
            switch (pc) {
            case 0x0008BB2Cu:
            case 0x0007A074u:
            case 0x00084F48u:
            case 0x00079268u:
            case 0x0009F930u:
            case 0x0009EC4Cu:
                ir.CallHostFunction(&TraceAceMenuExec, ir.Imm64((uint64_t)this), ir.Imm64(pc), ir.Imm64(is_thumb));
                break;
            default:
                break;
            }
        }
    }

    template <typename T>
    T MemoryRead(Dynarmic::A32::VAddr addr) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < KiB(4)) {
            LOG_ERROR("Invalid read of uint{}_t at address: 0x{:x}\n{}", sizeof(T) * 8, addr, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < KiB(4))
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return 0;
        }

        T ret = *ptr.get(*parent->mem);
        if (cpu->log_mem) {
            LOG_TRACE("Read uint{}_t at address: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, ret);
        }
        return ret;
    }

    uint8_t MemoryRead8(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint8_t>(addr);
    }

    uint16_t MemoryRead16(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint16_t>(addr);
    }

    uint32_t MemoryRead32(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint32_t>(addr);
    }

    uint64_t MemoryRead64(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint64_t>(addr);
    }

    template <typename T>
    void MemoryWrite(Dynarmic::A32::VAddr addr, T value) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < KiB(4)) {
            LOG_ERROR("Invalid write of uint{}_t at addr: 0x{:x}, val = 0x{:x}\n{}", sizeof(T) * 8, addr, value, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < KiB(4))
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return;
        }

        T *const host = ptr.get(*parent->mem);
        if (acxi_text_trace_enabled() && acxi_text_address(addr)) {
            static std::atomic<uint32_t> count{ 0 };
            const uint32_t index = count.fetch_add(1, std::memory_order_relaxed);
            if (index < 128) {
                LOG_ERROR("ACXI_TEXT_WRITE index={} thread={} bits={} addr=0x{:08X} old=0x{:X} new=0x{:X} pc=0x{:08X} lr=0x{:08X}",
                    index, parent->thread_id, sizeof(T) * 8, addr,
                    static_cast<uint64_t>(*host), static_cast<uint64_t>(value),
                    cpu->get_pc(), cpu->get_reg(14));
            }
        }
        *host = value;
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, value);
        }
    }

    void MemoryWrite8(Dynarmic::A32::VAddr addr, uint8_t value) override {
        MemoryWrite<uint8_t>(addr, value);
    }

    void MemoryWrite16(Dynarmic::A32::VAddr addr, uint16_t value) override {
        MemoryWrite<uint16_t>(addr, value);
    }

    void MemoryWrite32(Dynarmic::A32::VAddr addr, uint32_t value) override {
        MemoryWrite<uint32_t>(addr, value);
    }

    void MemoryWrite64(Dynarmic::A32::VAddr addr, uint64_t value) override {
        MemoryWrite<uint64_t>(addr, value);
    }

    template <typename T>
    bool MemoryWriteExclusive(Dynarmic::A32::VAddr addr, T value, T expected) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < KiB(4)) {
            LOG_ERROR("Invalid exclusive write of uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}\n{}", sizeof(T) * 8, addr, value, expected, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < KiB(4))
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return false;
        }

        if (acxi_text_trace_enabled() && acxi_text_address(addr)) {
            static std::atomic<uint32_t> count{ 0 };
            const uint32_t index = count.fetch_add(1, std::memory_order_relaxed);
            if (index < 128) {
                LOG_ERROR("ACXI_TEXT_EXCLUSIVE_WRITE index={} thread={} bits={} addr=0x{:08X} expected=0x{:X} new=0x{:X} pc=0x{:08X} lr=0x{:08X}",
                    index, parent->thread_id, sizeof(T) * 8, addr,
                    static_cast<uint64_t>(expected), static_cast<uint64_t>(value),
                    cpu->get_pc(), cpu->get_reg(14));
            }
        }
        auto result = Ptr<T>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}", sizeof(T) * 8, addr, value, expected);
        }
        return result;
    }

    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr addr, uint8_t value, uint8_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr addr, uint16_t value, uint16_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr addr, uint32_t value, uint32_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr addr, uint64_t value, uint64_t expected) override {
        return MemoryWriteExclusive(addr, value, expected); // Ptr<uint64_t>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
    }

    void InterpreterFallback(Dynarmic::A32::VAddr addr, size_t num_insts) override {
        LOG_ERROR("Unimplemented instruction at address {}:\n{}", log_hex(addr), save_context(*parent).description());
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::Breakpoint: {
            cpu->break_ = true;
            cpu->jit->HaltExecution();
            if (cpu->is_thumb_mode())
                cpu->set_pc(pc | 1);
            else
                cpu->set_pc(pc);
            break;
        }
        case Dynarmic::A32::Exception::WaitForInterrupt: {
            cpu->halted = true;
            cpu->jit->HaltExecution();
            break;
        }
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadInstruction:
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForEvent:
            break;
        case Dynarmic::A32::Exception::Yield:
            break;
        case Dynarmic::A32::Exception::UndefinedInstruction:
            if (acxi_text_trace_enabled()) {
                const uint32_t call_pc = 0x6A2E0u;
                const uint32_t call_word = MemoryRead32(call_pc);
                int32_t displacement = static_cast<int32_t>((call_word & 0x00FFFFFFu) << 8) >> 6;
                const uint32_t target = call_pc + 8u + static_cast<uint32_t>(displacement);
                LOG_ERROR("ACXI_TEXT_SNAPSHOT fault=0x{:08X} lr=0x{:08X} call=0x{:08X} word=0x{:08X} decoded_target=0x{:08X} callee_word=0x{:08X} return_word=0x{:08X} literal_word=0x{:08X}",
                    pc, cpu->get_reg(14), call_pc, call_word, target,
                    MemoryRead32(0x1FCE8u), MemoryRead32(0x1FD18u), MemoryRead32(0x3E804u));
            }
            LOG_WARN("Undefined instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::UnpredictableInstruction:
            LOG_WARN("Unpredictable instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::DecodeError: {
            LOG_WARN("Decode error at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        }
        default:
            LOG_WARN("Unknown exception {} Raised at pc = 0x{:x}", static_cast<size_t>(exception), pc);
            LOG_TRACE("at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
        }
    }

    void CallSVC(uint32_t svc) override {
        parent->svc_called = true;
        parent->svc = svc;
        cpu->jit->HaltExecution(Dynarmic::HaltReason::UserDefined8);
    }

    void AddTicks(uint64_t ticks) override {
        if (ticks > parent->ticks_remaining) {
            parent->ticks_remaining = 0;
            return;
        }
        parent->ticks_remaining -= ticks;
    }

    uint64_t GetTicksRemaining() override {
        return parent->ticks_remaining;
    }
};

Dynarmic::ExclusiveMonitor DynarmicCPU::shared_monitor(MAX_CORE_COUNT);

std::unique_ptr<Dynarmic::A32::Jit> DynarmicCPU::make_jit() {
    Dynarmic::A32::UserConfig config{};
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.callbacks = cb.get();
    if (parent->mem->use_page_table) {
        config.page_table = (log_mem || !cpu_opt) ? nullptr : reinterpret_cast<decltype(config.page_table)>(parent->mem->page_table.get());
        config.absolute_offset_page_table = true;
    } else if (!log_mem && cpu_opt) {
        config.fastmem_pointer = std::bit_cast<uintptr_t>(parent->mem->memory.get());
    }
    config.hook_hint_instructions = true;
    config.global_monitor = &shared_monitor;
    config.coprocessors[15] = cp15;
    config.processor_id = core_id;
    config.optimizations = cpu_opt ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
    config.enable_cycle_counting = true;

    if (dynarmic_jit_lifetime_trace_enabled()) {
        LOG_INFO("DYNARMIC_CPU_JIT_CREATE_BEGIN cpu={} guest_thread={} core={} cpu_opt={} use_page_table={} log_mem={} log_code={} page_table={} fastmem=0x{:X} host_thread=0x{:X}",
            static_cast<const void *>(this), parent->thread_id, core_id, cpu_opt,
            parent->mem->use_page_table, log_mem, log_code, static_cast<const void *>(config.page_table),
            config.fastmem_pointer.value_or(0), dynarmic_host_thread_tag());
    }
    auto created = std::make_unique<Dynarmic::A32::Jit>(config);
    if (dynarmic_jit_lifetime_trace_enabled()) {
        LOG_INFO("DYNARMIC_CPU_JIT_CREATE_END cpu={} guest_thread={} jit={} host_thread=0x{:X}",
            static_cast<const void *>(this), parent->thread_id, static_cast<const void *>(created.get()),
            dynarmic_host_thread_tag());
    }
    return created;
}

DynarmicCPU::DynarmicCPU(CPUState *state, std::size_t processor_id, bool cpu_opt)
    : parent(state)
    , cb(std::make_unique<ArmDynarmicCallback>(*state, *this))
    , cp15(std::make_shared<ArmDynarmicCP15>())
    , core_id(processor_id)
    , cpu_opt(cpu_opt) {
    jit = make_jit();
}

DynarmicCPU::~DynarmicCPU() {
    if (dynarmic_jit_lifetime_trace_enabled()) {
        LOG_INFO("DYNARMIC_CPU_DESTROY cpu={} guest_thread={} jit={} executing={} host_thread=0x{:X}",
            static_cast<const void *>(this), parent->thread_id, static_cast<const void *>(jit.get()),
            jit && jit->IsExecuting(), dynarmic_host_thread_tag());
    }
}

int DynarmicCPU::run() {
    halted = false;
    break_ = false;
    parent->svc_called = false;
    Dynarmic::HaltReason halt_reason;
    do {
        halt_reason = jit->Run();
    } while ((halt_reason == Dynarmic::HaltReason::Step) || (halt_reason == Dynarmic::HaltReason::CacheInvalidation));

    return halted;
}

int DynarmicCPU::step() {
    parent->svc_called = false;
    jit->Step();
    return 0;
}

bool DynarmicCPU::hit_breakpoint() {
    return break_;
}

void DynarmicCPU::trigger_breakpoint() {
    break_ = true;
    stop();
}

void DynarmicCPU::set_log_code(bool log) {
    if (log_code == log)
        return;

    if (dynarmic_jit_lifetime_trace_enabled()) {
        LOG_INFO("DYNARMIC_CPU_SET_LOG_CODE_BEGIN cpu={} guest_thread={} old_jit={} old_executing={} old_value={} new_value={} host_thread=0x{:X}",
            static_cast<const void *>(this), parent->thread_id, static_cast<const void *>(jit.get()),
            jit && jit->IsExecuting(), log_code, log, dynarmic_host_thread_tag());
    }
    log_code = log;
    jit = make_jit();
    if (dynarmic_jit_lifetime_trace_enabled()) {
        LOG_INFO("DYNARMIC_CPU_SET_LOG_CODE_END cpu={} guest_thread={} new_jit={} host_thread=0x{:X}",
            static_cast<const void *>(this), parent->thread_id, static_cast<const void *>(jit.get()),
            dynarmic_host_thread_tag());
    }
}

void DynarmicCPU::set_log_mem(bool log) {
    if (log_mem == log)
        return;

    if (dynarmic_jit_lifetime_trace_enabled()) {
        LOG_INFO("DYNARMIC_CPU_SET_LOG_MEM_BEGIN cpu={} guest_thread={} old_jit={} old_executing={} old_value={} new_value={} host_thread=0x{:X}",
            static_cast<const void *>(this), parent->thread_id, static_cast<const void *>(jit.get()),
            jit && jit->IsExecuting(), log_mem, log, dynarmic_host_thread_tag());
    }
    log_mem = log;
    jit = make_jit();
    if (dynarmic_jit_lifetime_trace_enabled()) {
        LOG_INFO("DYNARMIC_CPU_SET_LOG_MEM_END cpu={} guest_thread={} new_jit={} host_thread=0x{:X}",
            static_cast<const void *>(this), parent->thread_id, static_cast<const void *>(jit.get()),
            dynarmic_host_thread_tag());
    }
}

bool DynarmicCPU::get_log_code() {
    return log_code;
}

bool DynarmicCPU::get_log_mem() {
    return log_mem;
}

void DynarmicCPU::stop() {
    jit->HaltExecution();
}

uint32_t DynarmicCPU::get_reg(uint8_t idx) {
    return jit->Regs()[idx];
}

uint32_t DynarmicCPU::get_sp() {
    return jit->Regs()[13];
}

uint32_t DynarmicCPU::get_pc() {
    return jit->Regs()[15];
}

void DynarmicCPU::set_reg(uint8_t idx, uint32_t val) {
    jit->Regs()[idx] = val;
}

void DynarmicCPU::set_cpsr(uint32_t val) {
    jit->SetCpsr(val);
}

uint32_t DynarmicCPU::get_tpidruro() {
    return cp15->get_tpidruro();
}

void DynarmicCPU::set_tpidruro(uint32_t val) {
    cp15->set_tpidruro(val);
}

uint32_t DynarmicCPU::get_tpidrurw() {
    return cp15->get_tpidrurw();
}

void DynarmicCPU::set_tpidrurw(uint32_t val) {
    cp15->set_tpidrurw(val);
}

void DynarmicCPU::set_pc(uint32_t val) {
    if (val & 1) {
        set_cpsr(get_cpsr() | 0x20);
        val = val & 0xFFFFFFFE;
    } else {
        set_cpsr(get_cpsr() & 0xFFFFFFDF);
        val = val & 0xFFFFFFFC;
    }
    jit->Regs()[15] = val;
}

void DynarmicCPU::set_lr(uint32_t val) {
    jit->Regs()[14] = val;
}

void DynarmicCPU::set_sp(uint32_t val) {
    jit->Regs()[13] = val;
}

uint32_t DynarmicCPU::get_cpsr() {
    return jit->Cpsr();
}

uint32_t DynarmicCPU::get_fpscr() {
    return jit->Fpscr();
}

void DynarmicCPU::set_fpscr(uint32_t val) {
    jit->SetFpscr(val);
}

CPUContext DynarmicCPU::save_context() {
    CPUContext ctx;
    ctx.cpu_registers = jit->Regs();
    static_assert(sizeof(ctx.fpu_registers) == sizeof(jit->ExtRegs()));
    memcpy(ctx.fpu_registers.data(), jit->ExtRegs().data(), sizeof(ctx.fpu_registers));
    ctx.fpscr = jit->Fpscr();
    ctx.cpsr = jit->Cpsr();

    return ctx;
}

void DynarmicCPU::load_context(const CPUContext &ctx) {
    jit->Regs() = ctx.cpu_registers;
    static_assert(sizeof(ctx.fpu_registers) == sizeof(jit->ExtRegs()));
    memcpy(jit->ExtRegs().data(), ctx.fpu_registers.data(), sizeof(ctx.fpu_registers));
    jit->SetCpsr(ctx.cpsr);
    jit->SetFpscr(ctx.fpscr);
}

uint32_t DynarmicCPU::get_lr() {
    return jit->Regs()[14];
}

float DynarmicCPU::get_float_reg(uint8_t idx) {
    return std::bit_cast<float>(jit->ExtRegs()[idx]);
}

void DynarmicCPU::set_float_reg(uint8_t idx, float val) {
    jit->ExtRegs()[idx] = std::bit_cast<uint32_t>(val);
}

bool DynarmicCPU::is_thumb_mode() {
    return jit->Cpsr() & 0x20;
}

std::size_t DynarmicCPU::processor_id() const {
    return core_id;
}

void DynarmicCPU::invalidate_jit_cache(Address start, size_t length) {
    jit->InvalidateCacheRange(start, length);
}

void DynarmicCPU::clear_exclusive() {
    shared_monitor.ClearProcessor(core_id);
}
