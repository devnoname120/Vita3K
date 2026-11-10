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

#include "frame_host.h"
#include "../../interface.h"

#include <sdl-frontend/session.h>

#include <app/functions.h>
#include <app/session_controller.h>
#include <cpu/functions.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <audio/state.h>
#include <config/state.h>
#include <ctrl/functions.h>
#include <ctrl/state.h>
#include <emuenv/state.h>
#include <ime/functions.h>
#include <motion/event_handler.h>
#include <renderer/state.h>
#include <touch/functions.h>
#include <touch/state.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace sdl_frontend {

namespace {

struct ScriptedInputState {
    std::string path;
    uintmax_t consumed_bytes = 0;
    uintmax_t observed_size = static_cast<uintmax_t>(-1);
    bool controller_active = false;
    VirtualKeyboardState controller{};
};

void trace_guest_registers(EmuEnvState &emuenv) {
    const char *path = std::getenv("VITA3K_GUEST_PC_TRACE_FILE");
    if (!path || !*path)
        return;

    static uint64_t next_tick = 0;
    const uint64_t tick = SDL_GetTicks();
    if (tick < next_tick)
        return;
    next_tick = tick + 250;

    std::ofstream output(path, std::ios::app);
    if (!output)
        return;

    const std::lock_guard<std::mutex> kernel_lock(emuenv.kernel.mutex);
    for (const auto &[id, thread] : emuenv.kernel.threads) {
        if (!thread || !thread->cpu)
            continue;
        const std::lock_guard<std::mutex> thread_lock(thread->mutex);
        output << "tick=" << tick
               << " id=0x" << std::hex << static_cast<uint32_t>(id)
               << " status=0x" << static_cast<uint32_t>(thread->status)
               << " pc=0x" << read_pc(*thread->cpu)
               << " lr=0x" << read_lr(*thread->cpu)
               << " sp=0x" << read_sp(*thread->cpu)
               << " r0=0x" << read_reg(*thread->cpu, 0)
               << " r1=0x" << read_reg(*thread->cpu, 1)
               << " r2=0x" << read_reg(*thread->cpu, 2)
               << " r3=0x" << read_reg(*thread->cpu, 3)
               << std::dec << " name=" << thread->name << '\n';
    }
}

bool apply_scripted_input(EmuEnvState &emuenv, ScriptedInputState &state,
    const std::function<void()> &screenshot) {
    const auto apply_controller_state = [&]() {
        if (!state.controller_active)
            return;
        std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
        emuenv.ctrl.keyboard_state = state.controller;
    };
    if (state.path.empty())
        return true;

    std::error_code file_error;
    const uintmax_t file_size = std::filesystem::file_size(state.path, file_error);
    if (file_error)
        return true;
    if (file_size < state.consumed_bytes)
        state.consumed_bytes = 0;
    if (file_size != state.observed_size) {
        LOG_INFO("Scripted input size={} consumed={}", file_size, state.consumed_bytes);
        state.observed_size = file_size;
    }
    if (file_size == state.consumed_bytes) {
        apply_controller_state();
        return true;
    }

    std::ifstream file(state.path, std::ios::binary);
    if (!file)
        return true;
    file.seekg(static_cast<std::streamoff>(state.consumed_bytes));
    std::string pending(static_cast<size_t>(file_size - state.consumed_bytes), '\0');
    file.read(pending.data(), static_cast<std::streamsize>(pending.size()));
    pending.resize(static_cast<size_t>(file.gcount()));

    size_t start = 0;
    while (true) {
        const size_t end = pending.find('\n', start);
        if (end == std::string::npos)
            break;
        const std::string line = pending.substr(start, end - start);
        state.consumed_bytes += end - start + 1;
        start = end + 1;

        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command.empty() || command[0] == '#')
            continue;

        if (command == "touch_down" || command == "touch_move" || command == "touch_up") {
            float x = 0.f;
            float y = 0.f;
            if (!(input >> x >> y)) {
                LOG_ERROR("Invalid scripted touch command: {}", line);
                continue;
            }
            SDL_TouchFingerEvent finger{};
            finger.type = command == "touch_down" ? SDL_EVENT_FINGER_DOWN
                : command == "touch_move" ? SDL_EVENT_FINGER_MOTION
                                            : SDL_EVENT_FINGER_UP;
            finger.fingerID = 1;
            finger.x = std::clamp(x / 960.f, 0.f, 1.f);
            finger.y = std::clamp(y / 544.f, 0.f, 1.f);
            finger.pressure = command == "touch_up" ? 0.f : 1.f;
            handle_touch_event(emuenv.touch, finger);
            LOG_INFO("Scripted touch {} x={} y={}", command, x, y);
        } else if (command == "controller") {
            uint32_t buttons = 0;
            float lx = 0.f;
            float ly = 0.f;
            float rx = 0.f;
            float ry = 0.f;
            std::string button_text;
            if (!(input >> button_text >> lx >> ly >> rx >> ry)) {
                LOG_ERROR("Invalid scripted controller command: {}", line);
                continue;
            }
            try {
                buttons = static_cast<uint32_t>(std::stoul(button_text, nullptr, 0));
            } catch (...) {
                LOG_ERROR("Invalid scripted controller buttons: {}", line);
                continue;
            }
            state.controller = {};
            state.controller.buttons = buttons;
            state.controller.buttons_ext = buttons;
            state.controller.axes[0] = std::clamp(lx, -1.f, 1.f);
            state.controller.axes[1] = std::clamp(ly, -1.f, 1.f);
            state.controller.axes[2] = std::clamp(rx, -1.f, 1.f);
            state.controller.axes[3] = std::clamp(ry, -1.f, 1.f);
            state.controller_active = true;
            LOG_INFO("Scripted controller buttons=0x{:08X} axes={},{},{},{}",
                buttons, lx, ly, rx, ry);
        } else if (command == "controller_clear") {
            state.controller = {};
            state.controller_active = false;
            LOG_INFO("Scripted controller cleared");
        } else if (command == "screenshot") {
            screenshot();
            LOG_INFO("Scripted screenshot requested");
        } else if (command == "quit") {
            LOG_INFO("Scripted quit requested");
            return false;
        } else {
            LOG_ERROR("Unknown scripted input command: {}", line);
        }
    }

    apply_controller_state();
    return true;
}

// SDL_Quit at the end of a session stops the audio device, so the next session opens a new one
void reset_session_audio(EmuEnvState &emuenv) {
    if (!emuenv.audio.adapter)
        return;

    LOG_DEBUG("Resetting audio backend '{}' after SDL session shutdown", emuenv.audio.audio_backend);
    emuenv.audio.adapter.reset();
    emuenv.audio.audio_backend.clear();
}

void update_keyboard(EmuEnvState &emuenv, SDL_Window *window) {
    VirtualKeyboardState keyboard{};
    if (window && SDL_GetKeyboardFocus() == window && !is_any_ime_active(emuenv)) {
        int key_count = 0;
        const bool *keys = SDL_GetKeyboardState(&key_count);
        const auto held = [&](input::PhysicalKeyCode key) {
            const uint32_t usage = static_cast<uint32_t>(key);
            const uint32_t scancode = usage & 0xffff;
            // Only USB keyboard-page usages share SDL's numeric scancodes.
            return keys && key_count > 0 && (usage >> 16) == 0x07
                && scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_RGUI
                && scancode < static_cast<uint32_t>(key_count) && keys[scancode];
        };
        const auto pressed = [&](input::PhysicalKeyCode primary, input::PhysicalKeyCode alternate) {
            return held(primary) || held(alternate);
        };
        const auto &cfg = emuenv.cfg;
        const auto button = [&](input::PhysicalKeyCode primary, input::PhysicalKeyCode alternate,
                                uint32_t normal, uint32_t extended) {
            if (pressed(primary, alternate)) {
                keyboard.buttons |= normal;
                keyboard.buttons_ext |= extended;
            }
        };
        button(cfg.keyboard_button_select, cfg.keyboard_button_select_alt, SCE_CTRL_SELECT, SCE_CTRL_SELECT);
        button(cfg.keyboard_button_start, cfg.keyboard_button_start_alt, SCE_CTRL_START, SCE_CTRL_START);
        button(cfg.keyboard_button_up, cfg.keyboard_button_up_alt, SCE_CTRL_UP, SCE_CTRL_UP);
        button(cfg.keyboard_button_right, cfg.keyboard_button_right_alt, SCE_CTRL_RIGHT, SCE_CTRL_RIGHT);
        button(cfg.keyboard_button_down, cfg.keyboard_button_down_alt, SCE_CTRL_DOWN, SCE_CTRL_DOWN);
        button(cfg.keyboard_button_left, cfg.keyboard_button_left_alt, SCE_CTRL_LEFT, SCE_CTRL_LEFT);
        button(cfg.keyboard_button_triangle, cfg.keyboard_button_triangle_alt, SCE_CTRL_TRIANGLE, SCE_CTRL_TRIANGLE);
        button(cfg.keyboard_button_circle, cfg.keyboard_button_circle_alt, SCE_CTRL_CIRCLE, SCE_CTRL_CIRCLE);
        button(cfg.keyboard_button_cross, cfg.keyboard_button_cross_alt, SCE_CTRL_CROSS, SCE_CTRL_CROSS);
        button(cfg.keyboard_button_square, cfg.keyboard_button_square_alt, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE);
        button(cfg.keyboard_button_psbutton, cfg.keyboard_button_psbutton_alt, SCE_CTRL_PSBUTTON, SCE_CTRL_PSBUTTON);
        button(cfg.keyboard_button_l1, cfg.keyboard_button_l1_alt, SCE_CTRL_L, SCE_CTRL_L1);
        button(cfg.keyboard_button_r1, cfg.keyboard_button_r1_alt, SCE_CTRL_R, SCE_CTRL_R1);
        button(cfg.keyboard_button_l2, cfg.keyboard_button_l2_alt, 0, SCE_CTRL_L2);
        button(cfg.keyboard_button_r2, cfg.keyboard_button_r2_alt, 0, SCE_CTRL_R2);
        button(cfg.keyboard_button_l3, cfg.keyboard_button_l3_alt, 0, SCE_CTRL_L3);
        button(cfg.keyboard_button_r3, cfg.keyboard_button_r3_alt, 0, SCE_CTRL_R3);
        keyboard.axes[0] = static_cast<float>(
            static_cast<int>(pressed(cfg.keyboard_leftstick_right, cfg.keyboard_leftstick_right_alt))
            - static_cast<int>(pressed(cfg.keyboard_leftstick_left, cfg.keyboard_leftstick_left_alt)));
        keyboard.axes[1] = static_cast<float>(
            static_cast<int>(pressed(cfg.keyboard_leftstick_down, cfg.keyboard_leftstick_down_alt))
            - static_cast<int>(pressed(cfg.keyboard_leftstick_up, cfg.keyboard_leftstick_up_alt)));
        keyboard.axes[2] = static_cast<float>(
            static_cast<int>(pressed(cfg.keyboard_rightstick_right, cfg.keyboard_rightstick_right_alt))
            - static_cast<int>(pressed(cfg.keyboard_rightstick_left, cfg.keyboard_rightstick_left_alt)));
        keyboard.axes[3] = static_cast<float>(
            static_cast<int>(pressed(cfg.keyboard_rightstick_down, cfg.keyboard_rightstick_down_alt))
            - static_cast<int>(pressed(cfg.keyboard_rightstick_up, cfg.keyboard_rightstick_up_alt)));
    }

    std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
    emuenv.ctrl.keyboard_state = keyboard;
}

void update_mouse_input(EmuEnvState &emuenv, SDL_Window *window) {
    auto &touch = emuenv.touch;
    auto &overlay = emuenv.ctrl.overlay_mouse;
    const char *test_touch_path = std::getenv("VITA3K_TEST_TOUCH_FILE");
    static std::string announced_test_touch_path;
    if (test_touch_path && *test_touch_path && announced_test_touch_path != test_touch_path) {
        announced_test_touch_path = test_touch_path;
        LOG_INFO("Test front touch armed from {} window={} finger_count={}", test_touch_path, window != nullptr, touch.finger_count);
    }
    if (!window) {
        touch.renderer_focused = false;
        touch.mouse_button_left = false;
        touch.mouse_button_right = false;
        overlay.pressed.store(false, std::memory_order_relaxed);
        return;
    }

#ifdef __ANDROID__
    return;
#else
    if (touch.finger_count > 0 && !(test_touch_path && *test_touch_path))
        return;

    float window_x = 0.f;
    float window_y = 0.f;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&window_x, &window_y);
    int window_width = 0;
    int window_height = 0;
    int drawable_width = 0;
    int drawable_height = 0;
    SDL_GetWindowSize(window, &window_width, &window_height);
    SDL_GetWindowSizeInPixels(window, &drawable_width, &drawable_height);

    const float scale_x = window_width > 0 && drawable_width > 0
        ? static_cast<float>(drawable_width) / static_cast<float>(window_width)
        : 1.f;
    const float scale_y = window_height > 0 && drawable_height > 0
        ? static_cast<float>(drawable_height) / static_cast<float>(window_height)
        : 1.f;
    bool focused = SDL_GetKeyboardFocus() == window || SDL_GetMouseFocus() == window;

    struct TestTouchState {
        std::string path;
        uint64_t release_tick = 0;
        uint64_t generation = 0;
        unsigned phase = 0;
        float normalized_x = 0.8f;
        float normalized_y = 0.78f;
    };
    static TestTouchState test_touch;
    if (test_touch_path && *test_touch_path) {
        if (test_touch.path != test_touch_path) {
            test_touch = { test_touch_path };
        }
        const bool trigger_exists = std::filesystem::exists(test_touch.path);
        if (test_touch.phase == 3 && trigger_exists) {
            float trigger_x = 0.f;
            float trigger_y = 0.f;
            unsigned trigger_hold_ms = 0;
            uint64_t trigger_generation = 0;
            std::ifstream generation_probe(test_touch.path);
            if ((generation_probe >> trigger_x >> trigger_y >> trigger_hold_ms >> trigger_generation)
                && trigger_generation != test_touch.generation) {
                test_touch.phase = 0;
            }
        }
        if (test_touch.phase == 3 && !trigger_exists)
            test_touch.phase = 0;
        if (test_touch.phase == 0 && trigger_exists) {
            test_touch.normalized_x = 0.8f;
            test_touch.normalized_y = 0.78f;
            unsigned hold_ms = 250;
            unsigned requested_hold_ms = 0;
            uint64_t requested_generation = test_touch.generation + 1;
            std::ifstream trigger(test_touch.path);
            if (!(trigger >> test_touch.normalized_x >> test_touch.normalized_y)
                || test_touch.normalized_x < 0.f || test_touch.normalized_x > 1.f
                || test_touch.normalized_y < 0.f || test_touch.normalized_y > 1.f) {
                test_touch.normalized_x = 0.8f;
                test_touch.normalized_y = 0.78f;
                ++test_touch.generation;
            } else {
                if (trigger >> requested_hold_ms
                    && requested_hold_ms >= 50 && requested_hold_ms <= 10000) {
                    hold_ms = requested_hold_ms;
                }
                if (trigger >> requested_generation)
                    test_touch.generation = requested_generation;
                else
                    ++test_touch.generation;
            }
            test_touch.phase = 1;
            test_touch.release_tick = SDL_GetTicks() + hold_ms;
            LOG_INFO("Test front touch pressed from {} at ({}, {}) for {} ms", test_touch.path,
                test_touch.normalized_x, test_touch.normalized_y, hold_ms);
        }
        if (test_touch.phase == 1 && std::filesystem::exists(test_touch.path)) {
            float latest_x = test_touch.normalized_x;
            float latest_y = test_touch.normalized_y;
            std::ifstream latest(test_touch.path);
            if ((latest >> latest_x >> latest_y)
                && latest_x >= 0.f && latest_x <= 1.f
                && latest_y >= 0.f && latest_y <= 1.f) {
                test_touch.normalized_x = latest_x;
                test_touch.normalized_y = latest_y;
            }
        }
        if (test_touch.phase == 1) {
            window_x = static_cast<float>(window_width) * test_touch.normalized_x;
            window_y = static_cast<float>(window_height) * test_touch.normalized_y;
            focused = true;
            buttons |= SDL_BUTTON_LMASK;
            if (SDL_GetTicks() >= test_touch.release_tick)
                test_touch.phase = 2;
        } else if (test_touch.phase == 2) {
            window_x = static_cast<float>(window_width) * test_touch.normalized_x;
            window_y = static_cast<float>(window_height) * test_touch.normalized_y;
            focused = true;
            buttons &= ~SDL_BUTTON_LMASK;
            test_touch.phase = 3;
            LOG_INFO("Test front touch released from {}", test_touch.path);
        }
    }

    touch.mouse_x = window_x * scale_x;
    touch.mouse_y = window_y * scale_y;
    touch.renderer_focused = focused;
    touch.mouse_button_left = focused && (buttons & SDL_BUTTON_LMASK) != 0;
    touch.mouse_button_right = focused && (buttons & SDL_BUTTON_RMASK) != 0;

    overlay.x.store(window_width > 0 ? window_x * 960.f / window_width : window_x,
        std::memory_order_relaxed);
    overlay.y.store(window_height > 0 ? window_y * 544.f / window_height : window_y,
        std::memory_order_relaxed);
    overlay.pressed.store(touch.mouse_button_left, std::memory_order_relaxed);
#endif
}

void notify_ime_changed(const Hooks &hooks) {
    if (hooks.on_ime_changed)
        hooks.on_ime_changed();
}

// Returns whether the key was for the active IME
bool handle_ime_keydown(EmuEnvState &emuenv, const Hooks &hooks, const SDL_KeyboardEvent &event) {
    if (!is_any_ime_active(emuenv))
        return false;

    auto &ime = emuenv.ime;
    switch (event.key) {
    case SDLK_BACKSPACE: {
        const std::lock_guard<std::mutex> lock(ime.mutex);
        ime_backspace(ime);
        break;
    }
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        submit_current_ime(emuenv);
        break;
    case SDLK_ESCAPE:
        dismiss_current_ime(emuenv);
        break;
    case SDLK_LEFT: {
        const std::lock_guard<std::mutex> lock(ime.mutex);
        ime_cursor_left(ime);
        break;
    }
    case SDLK_RIGHT: {
        const std::lock_guard<std::mutex> lock(ime.mutex);
        ime_cursor_right(ime);
        break;
    }
    default:
        return false;
    }

    notify_ime_changed(hooks);
    return true;
}

void handle_ime_text_editing(EmuEnvState &emuenv, const Hooks &hooks, const char *text) {
    if (!is_any_ime_active(emuenv))
        return;

    {
        const std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        ime_set_preedit(emuenv.ime, string_utils::utf8_to_utf16(text ? text : ""));
    }
    notify_ime_changed(hooks);
}

void handle_ime_text_input(EmuEnvState &emuenv, const Hooks &hooks, const char *text) {
    if (!is_any_ime_active(emuenv) || !text || text[0] == '\0')
        return;

    std::string filtered_text;
    filtered_text.reserve(std::strlen(text));
    for (const char *ch = text; *ch != '\0'; ++ch) {
        if (*ch != '\n' && *ch != '\r')
            filtered_text.push_back(*ch);
    }

    if (filtered_text.empty())
        return;

    {
        const std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        ime_commit_text(emuenv.ime, string_utils::utf8_to_utf16(filtered_text));
    }
    notify_ime_changed(hooks);
}

void update_test_ime(EmuEnvState &emuenv, const Hooks &hooks) {
    const char *test_ime_path = std::getenv("VITA3K_TEST_IME_FILE");
    if (!test_ime_path || !*test_ime_path)
        return;

    struct TestImeState {
        std::string path;
        bool consumed = false;
    };
    static TestImeState test_ime;
    if (test_ime.path != test_ime_path)
        test_ime = { test_ime_path };

    const bool trigger_exists = std::filesystem::exists(test_ime.path);
    if (test_ime.consumed) {
        if (!trigger_exists)
            test_ime.consumed = false;
        return;
    }
    if (!trigger_exists || !is_any_ime_active(emuenv))
        return;

    std::ifstream trigger(test_ime.path, std::ios::binary);
    std::string text;
    if (!std::getline(trigger, text) || text.empty() || text.size() > 2048) {
        test_ime.consumed = true;
        LOG_WARN("Ignoring invalid test IME trigger from {}", test_ime.path);
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        emuenv.ime.str.clear();
        emuenv.ime.caretIndex = 0;
        emuenv.ime.edit_text = {};
        ime_commit_text(emuenv.ime, string_utils::utf8_to_utf16(text));
    }
    notify_ime_changed(hooks);
    test_ime.consumed = true;
    if (submit_current_ime(emuenv))
        LOG_INFO("Submitted test IME text from {}", test_ime.path);
    else
        LOG_WARN("Test IME became inactive before submission from {}", test_ime.path);
}

// Sets the attributes of the OpenGL context the renderer needs. Returns false if SDL refused one.
bool set_gl_attributes() {
#ifdef __ANDROID__
    const bool set = SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES)
        && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3)
        && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    const bool set = SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE)
        && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4)
        && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    if (!set || !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) || !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0))
        return false;
#ifndef NDEBUG
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
    return true;
}

} // namespace

int run(EmuEnvState &emuenv, app::AppSessionController &session, AppLaunchRequest launch_request, const Hooks &hooks) {
    int exit_code = 0;
    bool relaunch_requested = false;

    do {
        relaunch_requested = false;

        SDL_Window *window = nullptr;
        SDL_GLContext gl_context = nullptr;
        FrameHost frame_host(nullptr, &gl_context);
        std::optional<AppLaunchRequest> pending_launch_request;

        const auto cleanup_launch = [&](const app::AppSessionStopReason reason) {
            if (hooks.on_stop)
                hooks.on_stop();
            update_mouse_input(emuenv, nullptr);
            update_keyboard(emuenv, nullptr);
            session.stop(reason);
            reset_session_audio(emuenv);

            if (hooks.on_window)
                hooks.on_window(nullptr);
            if (window) {
                SDL_DestroyWindow(window);
                window = nullptr;
            }

            SDL_Quit();
        };

        LOG_INFO("Booting game '{}'", launch_request.app_path);

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_SENSOR | SDL_INIT_CAMERA)) {
            LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        refresh_controllers(emuenv.ctrl, emuenv);
        if (hooks.on_init)
            hooks.on_init();

        if (!session.begin_launch(launch_request, launch_request.reason != AppLaunchReason::LoadExec)) {
            LOG_ERROR("Could not find app '{}' in apps list.", launch_request.app_path);
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }
        if (hooks.on_launch)
            hooks.on_launch(launch_request.app_path);

        SDL_WindowFlags window_flags = 0;
#ifndef __ANDROID__
        window_flags |= SDL_WINDOW_RESIZABLE;
#endif
        if (emuenv.backend_renderer == renderer::Backend::OpenGL) {
            window_flags |= SDL_WINDOW_OPENGL;
            if (!set_gl_attributes()) {
                LOG_ERROR("Failed to configure OpenGL context attributes: {}", SDL_GetError());
                exit_code = -1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }
        } else if (emuenv.backend_renderer == renderer::Backend::Vulkan) {
            window_flags |= SDL_WINDOW_VULKAN;
        }

        SDL_PropertiesID window_props = SDL_CreateProperties();
        if (!window_props) {
            LOG_ERROR("SDL_CreateProperties failed: {}", SDL_GetError());
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        SDL_SetStringProperty(window_props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Vita3K");
        SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 960);
        SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 544);
        SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, window_flags);
        if (emuenv.backend_renderer == renderer::Backend::OpenGL)
            SDL_SetBooleanProperty(window_props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);

        window = SDL_CreateWindowWithProperties(window_props);
        SDL_DestroyProperties(window_props);
        if (!window) {
            LOG_ERROR("SDL_CreateWindowWithProperties failed: {}", SDL_GetError());
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }
        if (hooks.on_window)
            hooks.on_window(window);
        frame_host = FrameHost(window, &gl_context);

        if (emuenv.backend_renderer == renderer::Backend::OpenGL) {
            gl_context = SDL_GL_CreateContext(window);
            if (!gl_context) {
                LOG_ERROR("Failed to create OpenGL context: {}", SDL_GetError());
                exit_code = -1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }

            if (!SDL_GL_MakeCurrent(window, gl_context)) {
                LOG_ERROR("Failed to make OpenGL context current: {}", SDL_GetError());
                exit_code = -1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }

            if (!SDL_GL_SetSwapInterval(static_cast<int>(emuenv.cfg.current_config.v_sync)))
                LOG_WARN("Failed to set OpenGL swap interval: {}", SDL_GetError());
        }

        if (!session.initialize_renderer(frame_host)) {
            LOG_ERROR("Failed to initialise renderer.");
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (!session.initialize_runtime()) {
            LOG_ERROR("Failed late initialisation.");
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (!session.load_and_run()) {
            LOG_ERROR("Failed to load or start the app session.");
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (auto request = emuenv.take_app_launch_request())
            pending_launch_request = std::move(request);

        LOG_INFO("Game started: {} ({})", emuenv.current_app_title, launch_request.app_path);
        app::LaunchRuntimeMetrics runtime_metrics{};
        ScriptedInputState scripted_input;
        if (const char *script_path = std::getenv("VITA3K_SCRIPTED_INPUT"))
            scripted_input.path = script_path;
        LOG_INFO("Scripted input path='{}'", scripted_input.path);

        const char *capture_setting = std::getenv("VITA3K_WAW_CAPTURE");
        const bool capture_frames = capture_setting && std::strcmp(capture_setting, "1") == 0;
        unsigned capture_limit = 24;
        if (const char *capture_limit_setting = std::getenv("VITA3K_WAW_CAPTURE_LIMIT")) {
            char *capture_limit_end = nullptr;
            const unsigned long requested_capture_limit = std::strtoul(capture_limit_setting, &capture_limit_end, 10);
            if (capture_limit_end != capture_limit_setting && *capture_limit_end == '\0'
                && requested_capture_limit > 0 && requested_capture_limit <= 3600) {
                capture_limit = static_cast<unsigned>(requested_capture_limit);
            }
        }
        uint64_t next_capture_tick = SDL_GetTicks() + 5000;
        unsigned captured_frames = 0;
        const char *const exit_trace_value = std::getenv("VITA3K_SDL_EXIT_TRACE");
        const bool exit_trace = exit_trace_value && *exit_trace_value && std::strcmp(exit_trace_value, "0") != 0;
        const char *exit_reason = pending_launch_request ? "pending-before-loop" : nullptr;
        const auto record_exit = [&](const char *reason, uint32_t event_type, uint32_t window_id) {
            if (exit_reason)
                return;
            exit_reason = reason;
            if (exit_trace) {
                LOG_ERROR("SDL_SESSION_EXIT_REQUEST reason={} event=0x{:X} window={} flags=0x{:X} pending={} running={}",
                    reason, event_type, window_id, window ? SDL_GetWindowFlags(window) : 0,
                    pending_launch_request.has_value(), session.is_running());
            }
        };
        bool running = !pending_launch_request.has_value();
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    record_exit("sdl-quit", event.type, 0);
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    record_exit("window-close-requested", event.type, event.window.windowID);
                    running = false;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    handle_ime_keydown(emuenv, hooks, event.key);
                    break;

                case SDL_EVENT_TEXT_EDITING:
                    handle_ime_text_editing(emuenv, hooks, event.edit.text);
                    break;

                case SDL_EVENT_TEXT_INPUT:
                    handle_ime_text_input(emuenv, hooks, event.text.text);
                    break;

                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_MOTION:
                case SDL_EVENT_FINGER_UP:
                case SDL_EVENT_FINGER_CANCELED: {
                    handle_touch_event(emuenv.touch, event.tfinger);
                    auto &mouse = emuenv.ctrl.overlay_mouse;
                    mouse.x.store(event.tfinger.x * 960.f, std::memory_order_relaxed);
                    mouse.y.store(event.tfinger.y * 544.f, std::memory_order_relaxed);
                    mouse.pressed.store(event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION, std::memory_order_relaxed);
                    break;
                }

                case SDL_EVENT_GAMEPAD_ADDED:
                case SDL_EVENT_GAMEPAD_REMOVED:
                    refresh_controllers(emuenv.ctrl, emuenv);
                    break;

                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE && hooks.on_guide_button)
                        hooks.on_guide_button();
                    break;

                case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
                case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
                case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
                    handle_touchpad_event(emuenv.touch, event.gtouchpad);
                    break;

                case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                    handle_motion_event(emuenv, event.gsensor.sensor, event.gsensor);
                    break;
                case SDL_EVENT_SENSOR_UPDATE:
                    handle_motion_event(emuenv, SDL_GetSensorTypeForID(event.sensor.which), event.sensor);
                    break;

                default:
                    break;
                }
            }

            update_mouse_input(emuenv, window);
            update_keyboard(emuenv, window);
            update_test_ime(emuenv, hooks);
            if (!apply_scripted_input(emuenv, scripted_input, [&]() { take_screenshot(emuenv); })) {
                record_exit("scripted-quit", 0, 0);
                running = false;
            }
            trace_guest_registers(emuenv);

            if (!pending_launch_request) {
                if (auto request = emuenv.take_app_launch_request()) {
                    pending_launch_request = std::move(request);
                    record_exit("app-launch-request", 0, 0);
                    running = false;
                }
            }

            app::update_runtime_metrics(emuenv, runtime_metrics);

            if (!session.is_running()) {
                record_exit("session-not-running", 0, 0);
                running = false;
            }

            // Capture emulator output without relying on host desktop permissions.
            if (running && capture_frames && captured_frames < capture_limit && SDL_GetTicks() >= next_capture_tick) {
                take_screenshot(emuenv);
                ++captured_frames;
                next_capture_tick = SDL_GetTicks() + 5000;
            }

            if (running)
                SDL_Delay(16);
        }

        if (exit_trace) {
            LOG_ERROR("SDL_SESSION_EXIT reason={} pending={} running={}",
                exit_reason ? exit_reason : "loop-ended-without-reason",
                pending_launch_request.has_value(), session.is_running());
        }
        LOG_INFO("Shutting down game");

        if (pending_launch_request && pending_launch_request->reason == AppLaunchReason::ProcessExit) {
            exit_code = pending_launch_request->exit_code;
            LOG_INFO("The app exited with code {}", exit_code);
        } else if (pending_launch_request) {
            launch_request = std::move(*pending_launch_request);
            relaunch_requested = true;
            LOG_INFO("Relaunching in-process with self '{}'", launch_request.self_path);
        }

        cleanup_launch(relaunch_requested
                ? app::AppSessionStopReason::Relaunch
                : app::AppSessionStopReason::UserRequest);
    } while (relaunch_requested);

    return exit_code;
}

} // namespace sdl_frontend
