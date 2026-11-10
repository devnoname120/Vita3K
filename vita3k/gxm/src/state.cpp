// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <gxm/state.h>

#include <SDL3/SDL_thread.h>

void GxmState::deinit() {
    if (display_host_thread)
        SDL_WaitThread(display_host_thread, nullptr);
    display_host_thread = nullptr;

    {
        const std::lock_guard<std::mutex> lock(sync_objects_mutex);
        sync_objects.clear();
    }

    memory_mapped_regions.clear();
    display_queue.reset();
    params = {};
    display_queue_thread = 0;
    global_timestamp = 1;
    last_display_global = 0;
    notification_region = Ptr<uint32_t>(0);
    immediate_contexts.clear();
    last_immediate_context = 0;
    deferred_contexts.clear();
    render_targets.clear();
}
