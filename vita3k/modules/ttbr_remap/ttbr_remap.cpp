// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "mapping_policy.h"

#include <module/module.h>

#include <cpu/functions.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <util/log.h>
#include <util/tracy.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

TRACY_MODULE_NAME(ttbr_remap);

namespace {
constexpr uint32_t TTBR_REMAP_PERM_R = 1u;
constexpr uint32_t TTBR_REMAP_PERM_W = 2u;
constexpr uint32_t TTBR_REMAP_PERM_X = 4u;
constexpr uint32_t TTBR_REMAP_EXIT_SVC = 1u;
constexpr uint32_t TTBR_REMAP_EXIT_DABT = 4u;
constexpr uint32_t TTBR_REMAP_EXIT_TIMER = 5u;
constexpr uint32_t TTBR_REMAP_TIMER_HZ = 1000000u;
constexpr uint64_t TTBR_REMAP_HLE_TICKS_PER_US = 512u;
constexpr uint32_t TTBR_REMAP_EDIT_MAP = 1u;
constexpr uint32_t TTBR_REMAP_EDIT_UNMAP = 2u;
constexpr uint32_t TTBR_REMAP_EDIT_PROTECT = 3u;
constexpr uint32_t TTBR_REMAP_EDIT_SYNC_CODE = 4u;
constexpr uint32_t TTBR_REMAP_CREATE_LOW_OVERLAP = 1u;
constexpr int TTBR_REMAP_ERR_STATE = -1000;
constexpr int TTBR_REMAP_ERR_ARG = -1001;
constexpr int TTBR_REMAP_ERR_CORE = -1004;
constexpr int TTBR_REMAP_ERR_TABLE = -1005;
constexpr int TTBR_REMAP_ERR_PAGE = -1006;
constexpr int TTBR_REMAP_ERR_BUSY = -1009;
constexpr uint32_t PAGE_SIZE = 0x1000u;

struct TtbrRemapContext {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t tpidruro;
    uint32_t tpidrurw;
    uint32_t fpscr;
    uint32_t fpregs[64];
};

struct TtbrRemapRange {
    uint32_t user_va;
    uint32_t guest_va;
    uint32_t size;
    uint32_t perm;
};

struct TtbrRemapCreateInfo {
    uint32_t size;
    uint32_t flags;
};

struct TtbrRemapMapping {
    uint32_t size;
    TtbrRemapRange range;
};

struct TtbrRemapEditInfo {
    uint32_t size;
    uint32_t op;
    TtbrRemapRange range;
};

struct TtbrRemapRunOptions {
    uint32_t size;
    uint32_t flags;
    uint32_t quantum_us;
};

struct TtbrRemapExit {
    uint32_t size;
    uint32_t reason;
    uint32_t svc_number;
    uint32_t fsr;
    uint32_t far;
    uint32_t spsr;
    uint32_t raw_lr;
    uint32_t timer_hz;
    uint32_t timer_left;
};

struct TtbrRemapDebugState {
    uint32_t size;
    uint32_t phase;
    uint32_t cpsr;
    uint32_t pc;
    uint32_t irq_id;
};

static_assert(sizeof(TtbrRemapContext) == 0x150);
static_assert(sizeof(TtbrRemapMapping) == 20);
static_assert(sizeof(TtbrRemapEditInfo) == 24);
static_assert(sizeof(TtbrRemapExit) == 36);

struct TrackedRange {
    TtbrRemapRange range;
};

struct AliasAllocation {
    uint32_t guest_va;
    uint32_t size;
};

// One address space per handle. The loader's lanes (ttbr_remap_lane1..3) are
// separate libraries with the same function NIDs, so every lane lands here and
// each create needs its own handle. Vita3K has one guest address space, so an
// alias that several spaces map (the lanes apply every edit to each of them)
// is installed once and counted.
struct TtbrRemapSpace {
    bool mapping_failed = false;
    bool run_active = false;
    CPUContext last_context;
    std::vector<TrackedRange> mappings;
    std::vector<AliasAllocation> aliases;
};

struct SharedAlias {
    uint32_t guest_va;
    uint32_t user_va;
    uint32_t size;
    uint32_t refs;
};

// VITA3K_TTBR_REMAP_FRAME_WATCH: the caller's stack frame while its guest runs.
// Nothing but the guest should run on that thread then, so a word that changes
// (outside the context, exit and options records) was written by guest code,
// which on the console runs in another address space.
struct FrameWatch {
    uint32_t sp = 0;
    std::vector<uint8_t> bytes;
    std::vector<std::pair<uint32_t, uint32_t>> skip;
};

struct TtbrRemapHleState {
    std::map<SceUID, FrameWatch> frames;
    std::mutex mutex;
    uint32_t generation = 0;
    std::map<uint32_t, TtbrRemapSpace> spaces;
    std::vector<SharedAlias> aliases;
};

constexpr std::size_t TTBR_REMAP_HLE_MAX_SPACES = 4;

bool valid_guest_bytes(const MemState &mem, uint32_t address, uint32_t size) {
    if (!address || !size)
        return false;
    const uint64_t end = uint64_t(address) + size;
    if (end > 0x100000000ull)
        return false;
    uint64_t page = uint64_t(address) & ~(uint64_t(PAGE_SIZE) - 1u);
    while (page < end) {
        if (!is_valid_addr(mem, static_cast<Address>(page)))
            return false;
        page += PAGE_SIZE;
    }
    return true;
}

template <typename T>
int read_prefix(const MemState &mem, uint32_t address, uint32_t minimum, T &out) {
    if (!valid_guest_bytes(mem, address, sizeof(uint32_t)))
        return TTBR_REMAP_ERR_ARG;
    uint32_t declared_size = 0;
    std::memcpy(&declared_size, Ptr<const uint8_t>(address).get(mem), sizeof(declared_size));
    if (declared_size < minimum)
        return TTBR_REMAP_ERR_ARG;
    const uint32_t copy_size = std::min<uint32_t>(declared_size, sizeof(T));
    if (!valid_guest_bytes(mem, address, copy_size))
        return TTBR_REMAP_ERR_ARG;
    std::memset(&out, 0, sizeof(out));
    std::memcpy(&out, Ptr<const uint8_t>(address).get(mem), copy_size);
    return 0;
}

bool write_guest_bytes(MemState &mem, uint32_t address, const void *source, uint32_t size) {
    if (!valid_guest_bytes(mem, address, size))
        return false;
    std::memcpy(Ptr<uint8_t>(address).get(mem), source, size);
    return true;
}

constexpr uint32_t FRAME_WATCH_BYTES = 0x400;

bool frame_watch_enabled() {
    static const bool enabled = std::getenv("VITA3K_TTBR_REMAP_FRAME_WATCH") != nullptr;
    return enabled;
}

// VITA3K_TTBR_REMAP_RUN_TRACE=<thread id> logs every run of that thread (0: all).
bool run_trace_enabled(SceUID thread) {
    static const int traced = [] {
        const char *value = std::getenv("VITA3K_TTBR_REMAP_RUN_TRACE");
        return value ? std::atoi(value) : -1;
    }();
    return traced == 0 || traced == thread;
}

void frame_watch_check(MemState &mem, TtbrRemapHleState &state, SceUID runner, const CPUContext &guest, int reason) {
    for (auto &[owner, frame] : state.frames) {
        if (!valid_guest_bytes(mem, frame.sp, FRAME_WATCH_BYTES))
            continue;
        const uint8_t *now = Ptr<const uint8_t>(frame.sp).get(mem);
        for (uint32_t off = 0; off < FRAME_WATCH_BYTES; off += 4) {
            const uint32_t addr = frame.sp + off;
            bool skipped = false;
            for (const auto &[start, size] : frame.skip)
                skipped |= addr >= start && addr < start + size;
            if (skipped || std::memcmp(now + off, frame.bytes.data() + off, 4) == 0)
                continue;
            uint32_t before, after;
            std::memcpy(&before, frame.bytes.data() + off, 4);
            std::memcpy(&after, now + off, 4);
            LOG_WARN("ttbr_remap FRAME_WATCH owner={} sp={} addr={} off=0x{:X} old={} new={} runner={} guest pc={} sp={} lr={} r0={} r1={} r2={} r3={} r4={} r7={} reason={}",
                owner, log_hex(frame.sp), log_hex(addr), off, log_hex(before), log_hex(after), runner,
                log_hex(guest.get_pc()), log_hex(guest.cpu_registers[13]), log_hex(guest.cpu_registers[14]),
                log_hex(guest.cpu_registers[0]), log_hex(guest.cpu_registers[1]), log_hex(guest.cpu_registers[2]),
                log_hex(guest.cpu_registers[3]), log_hex(guest.cpu_registers[4]), log_hex(guest.cpu_registers[7]), reason);
        }
        std::memcpy(frame.bytes.data(), now, FRAME_WATCH_BYTES);
    }
}

bool range_shape_valid(const TtbrRemapRange &range, bool need_user, bool need_perm) {
    const uint64_t guest_end = uint64_t(range.guest_va) + range.size;
    const uint64_t user_end = uint64_t(range.user_va) + range.size;
    if (!range.size || (range.guest_va & (PAGE_SIZE - 1u)) || (range.size & (PAGE_SIZE - 1u)) || guest_end > 0x100000000ull)
        return false;
    if (need_user && ((range.user_va & (PAGE_SIZE - 1u)) || user_end > 0x100000000ull))
        return false;
    if (need_perm && (!(range.perm & TTBR_REMAP_PERM_R) || (range.perm & ~7u)))
        return false;
    return true;
}

bool overlaps(uint32_t a_start, uint32_t a_size, uint32_t b_start, uint32_t b_size) {
    const uint64_t a_end = uint64_t(a_start) + a_size;
    const uint64_t b_end = uint64_t(b_start) + b_size;
    return uint64_t(a_start) < b_end && uint64_t(b_start) < a_end;
}

bool page_is_mapped(const TtbrRemapSpace &state, uint32_t page) {
    return std::any_of(state.mappings.begin(), state.mappings.end(), [page](const TrackedRange &mapping) {
        return uint64_t(page) >= mapping.range.guest_va && uint64_t(page) < uint64_t(mapping.range.guest_va) + mapping.range.size;
    });
}

bool acquire_alias(MemState &mem, TtbrRemapHleState &shared, const TtbrRemapRange &range) {
    for (auto &alias : shared.aliases) {
        if (alias.guest_va == range.guest_va && alias.size == range.size && alias.user_va == range.user_va) {
            alias.refs++;
            return true;
        }
        if (overlaps(alias.guest_va, alias.size, range.guest_va, range.size)) {
            LOG_ERROR("ttbr_remap HLE alias guest={} size={} user={} conflicts with guest={} size={} user={}",
                log_hex(range.guest_va), log_hex(range.size), log_hex(range.user_va),
                log_hex(alias.guest_va), log_hex(alias.size), log_hex(alias.user_va));
            return false;
        }
    }
    if (!add_alias_mapping(mem, range.guest_va, range.user_va, range.size))
        return false;
    shared.aliases.push_back({ range.guest_va, range.user_va, range.size, 1 });
    return true;
}

bool release_alias(MemState &mem, TtbrRemapHleState &shared, uint32_t guest_va, uint32_t size) {
    const auto alias = std::find_if(shared.aliases.begin(), shared.aliases.end(),
        [guest_va, size](const SharedAlias &candidate) {
            return candidate.guest_va == guest_va && candidate.size == size;
        });
    if (alias == shared.aliases.end())
        return false;
    if (--alias->refs)
        return true;
    shared.aliases.erase(alias);
    return remove_alias_mapping(mem, guest_va, size);
}

int map_range(EmuEnvState &emuenv, TtbrRemapHleState &shared, TtbrRemapSpace &state, const TtbrRemapRange &range) {
    if (!range_shape_valid(range, true, true) || !valid_guest_bytes(emuenv.mem, range.user_va, range.size))
        return TTBR_REMAP_ERR_PAGE;

    for (const auto &mapping : state.mappings) {
        if (!overlaps(range.guest_va, range.size, mapping.range.guest_va, mapping.range.size))
            continue;
        if (std::memcmp(&range, &mapping.range, sizeof(range)) == 0)
            return 0;
        if (!ttbr_remap_overlapping_mapping_supported(mapping.range, range))
            return TTBR_REMAP_ERR_ARG;
    }

    if (range.user_va != range.guest_va) {
        if (!acquire_alias(emuenv.mem, shared, range))
            return TTBR_REMAP_ERR_TABLE;
        state.aliases.push_back({ range.guest_va, range.size });
    }
    state.mappings.push_back({ range });
    emuenv.kernel.invalidate_jit_cache(range.guest_va, range.size);
    LOG_INFO("ttbr_remap HLE map user={} guest={} size={} perm=0x{:X}",
        log_hex(range.user_va), log_hex(range.guest_va), log_hex(range.size), range.perm);
    return 0;
}

int unmap_range(EmuEnvState &emuenv, TtbrRemapHleState &shared, TtbrRemapSpace &state, const TtbrRemapRange &range) {
    if (!range_shape_valid(range, false, false))
        return TTBR_REMAP_ERR_ARG;

    std::vector<TrackedRange> replacement;
    replacement.reserve(state.mappings.size() + 1);
    const uint64_t remove_start = range.guest_va;
    const uint64_t remove_end = remove_start + range.size;

    for (const auto &mapping : state.mappings) {
        const uint64_t map_start = mapping.range.guest_va;
        const uint64_t map_end = map_start + mapping.range.size;
        if (remove_start >= map_end || map_start >= remove_end) {
            replacement.push_back(mapping);
            continue;
        }

        if (remove_start <= map_start && remove_end >= map_end && mapping.range.user_va != mapping.range.guest_va) {
            const auto alias = std::find_if(state.aliases.begin(), state.aliases.end(),
                [&mapping](const AliasAllocation &candidate) {
                    return candidate.guest_va == mapping.range.guest_va && candidate.size == mapping.range.size;
                });
            if (alias != state.aliases.end()) {
                if (!release_alias(emuenv.mem, shared, alias->guest_va, alias->size))
                    return TTBR_REMAP_ERR_STATE;
                state.aliases.erase(alias);
            }
        }

        if (remove_start > map_start) {
            TtbrRemapRange left = mapping.range;
            left.size = static_cast<uint32_t>(remove_start - map_start);
            replacement.push_back({ left });
        }
        if (remove_end < map_end) {
            TtbrRemapRange right = mapping.range;
            const uint32_t delta = static_cast<uint32_t>(remove_end - map_start);
            right.user_va += delta;
            right.guest_va += delta;
            right.size = static_cast<uint32_t>(map_end - remove_end);
            replacement.push_back({ right });
        }
    }

    state.mappings = std::move(replacement);
    emuenv.kernel.invalidate_jit_cache(range.guest_va, range.size);
    return 0;
}

int protect_range(const TtbrRemapSpace &state, const TtbrRemapRange &range) {
    if (!range_shape_valid(range, false, true))
        return TTBR_REMAP_ERR_ARG;
    for (uint64_t page = range.guest_va; page < uint64_t(range.guest_va) + range.size; page += PAGE_SIZE) {
        if (!page_is_mapped(state, static_cast<uint32_t>(page)))
            return TTBR_REMAP_ERR_ARG;
    }
    return 0;
}

int sync_code(EmuEnvState &emuenv, const TtbrRemapSpace &state, const TtbrRemapRange &range) {
    if (!range.size || (range.user_va & (PAGE_SIZE - 1u)) || (range.size & (PAGE_SIZE - 1u)) || uint64_t(range.user_va) + range.size > 0x100000000ull || !valid_guest_bytes(emuenv.mem, range.user_va, range.size))
        return TTBR_REMAP_ERR_ARG;

    const uint64_t sync_start = range.user_va;
    const uint64_t sync_end = sync_start + range.size;
    for (const auto &mapping : state.mappings) {
        const uint64_t user_start = mapping.range.user_va;
        const uint64_t user_end = user_start + mapping.range.size;
        const uint64_t overlap_start = std::max(sync_start, user_start);
        const uint64_t overlap_end = std::min(sync_end, user_end);
        if (overlap_start >= overlap_end)
            continue;
        const uint32_t guest_start = mapping.range.guest_va + static_cast<uint32_t>(overlap_start - user_start);
        emuenv.kernel.invalidate_jit_cache(guest_start,
            static_cast<uint32_t>(overlap_end - overlap_start));
    }
    return 0;
}

CPUContext to_cpu_context(const TtbrRemapContext &context) {
    CPUContext result;
    std::copy(std::begin(context.r), std::end(context.r), result.cpu_registers.begin());
    result.cpsr = context.cpsr;
    result.fpscr = context.fpscr;
    std::memcpy(result.fpu_registers.data(), context.fpregs, sizeof(context.fpregs));
    return result;
}

void from_cpu_context(const CPUContext &source, TtbrRemapContext &destination) {
    std::copy(source.cpu_registers.begin(), source.cpu_registers.end(), std::begin(destination.r));
    destination.cpsr = source.cpsr;
    destination.fpscr = source.fpscr;
    std::memcpy(destination.fpregs, source.fpu_registers.data(), sizeof(destination.fpregs));
}
} // namespace

LIBRARY_INIT(ttbr_remap) {
    emuenv.kernel.obj_store.create<TtbrRemapHleState>();
}

EXPORT(int, ttbr_remap_version) {
    return 2;
}

EXPORT(int, ttbr_remap_create, Ptr<const TtbrRemapCreateInfo> user_info, Ptr<uint32_t> user_handle) {
    TRACY_FUNC(ttbr_remap_create, user_info, user_handle);
    TtbrRemapCreateInfo info;
    int result = read_prefix(emuenv.mem, user_info.address(), 8, info);
    uint32_t zero = 0;
    if (result < 0 || !write_guest_bytes(emuenv.mem, user_handle.address(), &zero, sizeof(zero)))
        return RET_ERROR(TTBR_REMAP_ERR_ARG);
    if (info.flags & ~TTBR_REMAP_CREATE_LOW_OVERLAP)
        return RET_ERROR(TTBR_REMAP_ERR_ARG);

    auto *state = emuenv.kernel.obj_store.get<TtbrRemapHleState>();
    const std::lock_guard<std::mutex> lock(state->mutex);
    if (state->spaces.size() >= TTBR_REMAP_HLE_MAX_SPACES || state->generation == std::numeric_limits<uint32_t>::max())
        return RET_ERROR(TTBR_REMAP_ERR_BUSY);

    const uint32_t handle = ++state->generation;
    if (!write_guest_bytes(emuenv.mem, user_handle.address(), &handle, sizeof(handle)))
        return RET_ERROR(TTBR_REMAP_ERR_ARG);
    state->spaces.emplace(handle, TtbrRemapSpace{});
    LOG_INFO("ttbr_remap HLE create handle={} flags=0x{:X} spaces={}", handle, info.flags, state->spaces.size());
    return 0;
}

EXPORT(int, ttbr_remap_map, uint32_t handle, Ptr<const TtbrRemapMapping> user_mapping) {
    TRACY_FUNC(ttbr_remap_map, handle, user_mapping);
    TtbrRemapMapping mapping;
    int result = read_prefix(emuenv.mem, user_mapping.address(), 20, mapping);
    if (result < 0)
        return RET_ERROR(result);

    auto *state = emuenv.kernel.obj_store.get<TtbrRemapHleState>();
    const std::lock_guard<std::mutex> lock(state->mutex);
    const auto space = state->spaces.find(handle);
    if (space == state->spaces.end() || space->second.mapping_failed)
        return RET_ERROR(TTBR_REMAP_ERR_STATE);
    result = map_range(emuenv, *state, space->second, mapping.range);
    if (result < 0)
        space->second.mapping_failed = true;
    return result < 0 ? RET_ERROR(result) : result;
}

EXPORT(int, ttbr_remap_edit, uint32_t handle, Ptr<const TtbrRemapEditInfo> user_edit) {
    TRACY_FUNC(ttbr_remap_edit, handle, user_edit);
    TtbrRemapEditInfo edit;
    int result = read_prefix(emuenv.mem, user_edit.address(), 24, edit);
    if (result < 0)
        return RET_ERROR(result);

    auto *state = emuenv.kernel.obj_store.get<TtbrRemapHleState>();
    const std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->spaces.find(handle);
    if (found == state->spaces.end() || found->second.mapping_failed)
        return RET_ERROR(TTBR_REMAP_ERR_STATE);
    TtbrRemapSpace &space = found->second;

    switch (edit.op) {
    case TTBR_REMAP_EDIT_MAP:
        result = map_range(emuenv, *state, space, edit.range);
        break;
    case TTBR_REMAP_EDIT_UNMAP:
        result = unmap_range(emuenv, *state, space, edit.range);
        break;
    case TTBR_REMAP_EDIT_PROTECT:
        result = protect_range(space, edit.range);
        break;
    case TTBR_REMAP_EDIT_SYNC_CODE:
        result = sync_code(emuenv, space, edit.range);
        break;
    default:
        result = TTBR_REMAP_ERR_ARG;
        break;
    }
    if (result < 0)
        space.mapping_failed = true;
    return result < 0 ? RET_ERROR(result) : result;
}

EXPORT(int, ttbr_remap_run, uint32_t handle, Ptr<TtbrRemapContext> user_context,
    Ptr<const TtbrRemapRunOptions> user_options, Ptr<TtbrRemapExit> user_exit) {
    TRACY_FUNC(ttbr_remap_run, handle, user_context, user_options, user_exit);
    TtbrRemapRunOptions options;
    TtbrRemapExit exit;
    TtbrRemapContext context;
    int result = read_prefix(emuenv.mem, user_options.address(), 12, options);
    if (result < 0 || options.flags)
        return RET_ERROR(TTBR_REMAP_ERR_ARG);
    result = read_prefix(emuenv.mem, user_exit.address(), 8, exit);
    if (result < 0 || !valid_guest_bytes(emuenv.mem, user_context.address(), sizeof(context)))
        return RET_ERROR(TTBR_REMAP_ERR_ARG);
    std::memcpy(&context, user_context.get(emuenv.mem), sizeof(context));
    const uint32_t output_size = std::min<uint32_t>(exit.size, sizeof(exit));
    exit.size = output_size;
    if (!write_guest_bytes(emuenv.mem, user_context.address(), &context, sizeof(context)) || !write_guest_bytes(emuenv.mem, user_exit.address(), &exit, output_size))
        return RET_ERROR(TTBR_REMAP_ERR_ARG);

    auto *state = emuenv.kernel.obj_store.get<TtbrRemapHleState>();
    const auto thread = emuenv.kernel.get_thread(thread_id);
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        const auto space = state->spaces.find(handle);
        if (space == state->spaces.end() || space->second.mapping_failed || space->second.run_active)
            return RET_ERROR(TTBR_REMAP_ERR_STATE);
        if (!thread || !thread->cpu)
            return RET_ERROR(TTBR_REMAP_ERR_CORE);
        space->second.run_active = true;
        if (frame_watch_enabled()) {
            FrameWatch frame;
            frame.sp = read_sp(*thread->cpu);
            if (valid_guest_bytes(emuenv.mem, frame.sp, FRAME_WATCH_BYTES)) {
                frame.bytes.resize(FRAME_WATCH_BYTES);
                std::memcpy(frame.bytes.data(), Ptr<const uint8_t>(frame.sp).get(emuenv.mem), FRAME_WATCH_BYTES);
                frame.skip = { { user_context.address(), uint32_t(sizeof(context)) }, { user_exit.address(), output_size }, { user_options.address(), 12u } };
                state->frames[thread_id] = std::move(frame);
            }
        }
    }

    if (run_trace_enabled(thread_id))
        LOG_WARN("ttbr_remap RUN_TRACE enter thread={} handle={} pc={} sp={} lr={} cpsr={} host_sp={} host_pc={} host_lr={} quantum={}",
            thread_id, handle, log_hex(context.r[15]), log_hex(context.r[13]), log_hex(context.r[14]), log_hex(context.cpsr),
            log_hex(read_sp(*thread->cpu)), log_hex(read_pc(*thread->cpu)), log_hex(read_lr(*thread->cpu)), options.quantum_us);
    // Spaces run concurrently on their own threads; the guest code may block
    // on another lane, so no lock is held while it runs.
    const uint64_t tick_budget = options.quantum_us
        ? static_cast<uint64_t>(options.quantum_us) * TTBR_REMAP_HLE_TICKS_PER_US
        : CPU_RUN_UNBOUNDED_TICKS;
    CPUContextRunResult run = run_context_until_trap(
        *thread->cpu, to_cpu_context(context), context.tpidruro, context.tpidrurw,
        tick_budget);
    // An unbounded run has no timer exit to report a halt with. A halt that
    // leaves the thread runnable (another thread invalidating JIT code, say)
    // is invisible on hardware, where it is an interrupt the kernel returns
    // from, so the guest simply continues.
    while (!options.quantum_us && run.reason == CPUContextRunReason::Error && run.cpu_result == 0
        && thread->can_resume_nested_run()) {
        run = run_context_until_trap(*thread->cpu, run.context, run.tpidruro, run.tpidrurw, tick_budget);
    }
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        const auto space = state->spaces.find(handle);
        if (space != state->spaces.end()) {
            space->second.run_active = false;
            space->second.last_context = run.context;
        }
        if (frame_watch_enabled()) {
            frame_watch_check(emuenv.mem, *state, thread_id, run.context, int(run.reason));
            state->frames.erase(thread_id);
        }
    }
    if (run_trace_enabled(thread_id))
        LOG_WARN("ttbr_remap RUN_TRACE exit thread={} handle={} pc={} sp={} lr={} cpsr={} reason={} result={} svc={} fault={}",
            thread_id, handle, log_hex(run.context.get_pc()), log_hex(run.context.cpu_registers[13]), log_hex(run.context.cpu_registers[14]),
            log_hex(run.context.cpsr), int(run.reason), run.cpu_result, log_hex(run.svc), log_hex(run.fault_address));
    from_cpu_context(run.context, context);
    context.tpidruro = run.tpidruro;
    context.tpidrurw = run.tpidrurw;

    std::memset(reinterpret_cast<uint8_t *>(&exit) + sizeof(uint32_t), 0,
        sizeof(exit) - sizeof(uint32_t));
    exit.size = output_size;
    exit.spsr = context.cpsr;
    exit.raw_lr = context.r[15];
    if (run.reason == CPUContextRunReason::Svc) {
        exit.reason = TTBR_REMAP_EXIT_SVC;
        exit.svc_number = run.svc;
    } else if (run.reason == CPUContextRunReason::Abort) {
        exit.reason = TTBR_REMAP_EXIT_DABT;
        exit.far = run.fault_address;
        exit.fsr = run.fault_is_write ? 1u << 11 : 0;
    } else if (ttbr_remap_should_emit_timer_exit(
                   run.reason == CPUContextRunReason::Error,
                   run.cpu_result,
                   options.quantum_us,
                   thread->can_resume_nested_run())) {
        exit.reason = TTBR_REMAP_EXIT_TIMER;
        exit.timer_hz = TTBR_REMAP_TIMER_HZ;
        exit.timer_left = 0;
        LOG_DEBUG("ttbr_remap HLE resumed benign stop as timer pc={} quantum_us={}",
            log_hex(context.r[15]), options.quantum_us);
    } else {
        LOG_ERROR("ttbr_remap HLE guest run stopped without a supported trap pc={} result={}",
            log_hex(context.r[15]), run.cpu_result);
        return RET_ERROR(TTBR_REMAP_ERR_CORE);
    }

    if (!write_guest_bytes(emuenv.mem, user_context.address(), &context, sizeof(context)) || !write_guest_bytes(emuenv.mem, user_exit.address(), &exit, output_size))
        return RET_ERROR(TTBR_REMAP_ERR_ARG);
    LOG_DEBUG("ttbr_remap HLE run reason={} svc={} pc={}",
        exit.reason, exit.svc_number, log_hex(context.r[15]));
    return 0;
}

EXPORT(int, ttbr_remap_destroy, uint32_t handle) {
    TRACY_FUNC(ttbr_remap_destroy, handle);
    auto *state = emuenv.kernel.obj_store.get<TtbrRemapHleState>();
    const std::lock_guard<std::mutex> lock(state->mutex);
    const auto space = state->spaces.find(handle);
    if (space == state->spaces.end() || space->second.run_active)
        return RET_ERROR(TTBR_REMAP_ERR_STATE);

    int result = 0;
    for (const auto &alias : space->second.aliases) {
        if (!release_alias(emuenv.mem, *state, alias.guest_va, alias.size))
            result = TTBR_REMAP_ERR_STATE;
    }
    state->spaces.erase(space);
    return result < 0 ? RET_ERROR(result) : result;
}

EXPORT(int, ttbr_remap_debug_state, uint32_t handle, Ptr<TtbrRemapDebugState> user_out) {
    TRACY_FUNC(ttbr_remap_debug_state, handle, user_out);
    TtbrRemapDebugState out;
    int result = read_prefix(emuenv.mem, user_out.address(), 20, out);
    if (result < 0)
        return RET_ERROR(result);
    const uint32_t output_size = std::min<uint32_t>(out.size, sizeof(out));

    auto *state = emuenv.kernel.obj_store.get<TtbrRemapHleState>();
    const std::unique_lock<std::mutex> lock(state->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return RET_ERROR(TTBR_REMAP_ERR_BUSY);
    const auto space = state->spaces.find(handle);
    if (space == state->spaces.end())
        return RET_ERROR(TTBR_REMAP_ERR_STATE);

    out.size = output_size;
    out.phase = space->second.run_active ? 1u : 0u;
    out.cpsr = space->second.last_context.cpsr;
    out.pc = space->second.last_context.get_pc();
    out.irq_id = 0;
    return write_guest_bytes(emuenv.mem, user_out.address(), &out, output_size)
        ? 0
        : RET_ERROR(TTBR_REMAP_ERR_ARG);
}
