from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "vita3k/sdl-frontend/src/session.cpp").read_text()
ctrl = (ROOT / "vita3k/ctrl/src/ctrl.cpp").read_text()

def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

start = source.index("            SDL_Event event;", source.index("while (running)"))
end = source.index("            if (!pending_launch_request)", start)
pump = source[start:end]
updater = function(source, "void update_keyboard(") if "void update_keyboard(" in source else ""
consumer = function(ctrl, "static void apply_keyboard(")

prelude = r"""
#include <config/state.h>
#include <ctrl/state.h>
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>

struct EmuEnvState { Config cfg; CtrlState ctrl; int touch{}; bool ime_active{}; };
struct Hooks { std::function<void()> on_guide_button; };
static std::array<bool, SDL_SCANCODE_COUNT> held{};
static std::deque<SDL_Event> events;
static SDL_Window *test_window = reinterpret_cast<SDL_Window *>(uintptr_t(1));
static SDL_Window *focus = test_window;
static int key_count = SDL_SCANCODE_COUNT;
static bool null_keyboard = false;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)

extern "C" const bool *SDL_GetKeyboardState(int *count) {
    *count = key_count;
    return null_keyboard ? nullptr : held.data();
}
extern "C" SDL_Window *SDL_GetKeyboardFocus() { return focus; }
extern "C" SDL_SensorType SDL_GetSensorTypeForID(SDL_SensorID) { return SDL_SENSOR_INVALID; }
extern "C" bool SDL_PollEvent(SDL_Event *out) {
    if (events.empty()) return false;
    *out = events.front();
    events.pop_front();
    if ((out->type == SDL_EVENT_KEY_DOWN || out->type == SDL_EVENT_KEY_UP)
        && out->key.scancode > SDL_SCANCODE_UNKNOWN && out->key.scancode < SDL_SCANCODE_COUNT)
        held[out->key.scancode] = out->type == SDL_EVENT_KEY_DOWN;
    return true;
}
static bool is_any_ime_active(EmuEnvState &e) { return e.ime_active; }
static bool handle_ime_keydown(EmuEnvState &, const Hooks &, const SDL_KeyboardEvent &) { return false; }
template<class... T> static void handle_ime_text_editing(T&&...) {}
template<class... T> static void handle_ime_text_input(T&&...) {}
template<class... T> static void handle_touch_event(T&&...) {}
template<class... T> static void handle_touchpad_event(T&&...) {}
template<class... T> static void handle_motion_event(T&&...) {}
template<class... T> static void refresh_controllers(T&&...) {}
"""

tests = r"""
static void pump(EmuEnvState &emuenv, SDL_Window *window = test_window) {
    bool running = true;
    Hooks hooks;
@PUMP@
}
static void key(EmuEnvState &e, SDL_Scancode scancode, bool pressed, bool repeat = false) {
    SDL_Event event{};
    event.type = pressed ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.key = SDLK_Z;
    event.key.repeat = repeat;
    events.push_back(event);
    pump(e);
}
static void buttons(EmuEnvState &e, uint32_t ordinary, uint32_t extended) {
    for (bool ext : {false, true}) {
        uint32_t mask = 0;
        float axes[4]{};
        apply_keyboard(&mask, axes, ext, e);
        if (mask != (ext ? extended : ordinary)) std::fprintf(stderr, "check=%u ext=%d actual=%08x expected=%08x\n", checks+1, ext, mask, ext ? extended : ordinary);
        CHECK(mask == (ext ? extended : ordinary));
    }
}
int main() {
    EmuEnvState e;
    for (auto sc : {SDL_SCANCODE_DOWN, SDL_SCANCODE_UP, SDL_SCANCODE_X}) {
        const auto bit = sc == SDL_SCANCODE_DOWN ? SCE_CTRL_DOWN : sc == SDL_SCANCODE_UP ? SCE_CTRL_UP : SCE_CTRL_CROSS;
        key(e, sc, true);
        buttons(e, bit, bit);
        key(e, sc, false);
        buttons(e, 0, 0);
    }
    struct Binding { SDL_Scancode key; uint32_t normal; uint32_t extended; };
    for (const auto &b : std::array{
        Binding{SDL_SCANCODE_RETURN, SCE_CTRL_START, SCE_CTRL_START},
        Binding{SDL_SCANCODE_RSHIFT, SCE_CTRL_SELECT, SCE_CTRL_SELECT},
        Binding{SDL_SCANCODE_LEFT, SCE_CTRL_LEFT, SCE_CTRL_LEFT},
        Binding{SDL_SCANCODE_RIGHT, SCE_CTRL_RIGHT, SCE_CTRL_RIGHT},
        Binding{SDL_SCANCODE_C, SCE_CTRL_CIRCLE, SCE_CTRL_CIRCLE},
        Binding{SDL_SCANCODE_Z, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE},
        Binding{SDL_SCANCODE_V, SCE_CTRL_TRIANGLE, SCE_CTRL_TRIANGLE},
        Binding{SDL_SCANCODE_Q, SCE_CTRL_L, SCE_CTRL_L1},
        Binding{SDL_SCANCODE_E, SCE_CTRL_R, SCE_CTRL_R1},
        Binding{SDL_SCANCODE_U, 0, SCE_CTRL_L2},
        Binding{SDL_SCANCODE_O, 0, SCE_CTRL_R2},
        Binding{SDL_SCANCODE_F, 0, SCE_CTRL_L3},
        Binding{SDL_SCANCODE_H, 0, SCE_CTRL_R3}}) {
        key(e, b.key, true); buttons(e, b.normal, b.extended);
        key(e, b.key, false); buttons(e, 0, 0);
    }
    e.cfg.keyboard_button_cross_alt = input::PhysicalKeyCode::KeyB;
    key(e, SDL_SCANCODE_X, true);
    key(e, SDL_SCANCODE_B, true);
    key(e, SDL_SCANCODE_X, false);
    buttons(e, SCE_CTRL_CROSS, SCE_CTRL_CROSS);
    key(e, SDL_SCANCODE_B, false); buttons(e, 0, 0);
    e.cfg.keyboard_button_cross = input::PhysicalKeyCode::ArrowDown;
    key(e, SDL_SCANCODE_DOWN, true);
    buttons(e, SCE_CTRL_DOWN | SCE_CTRL_CROSS, SCE_CTRL_DOWN | SCE_CTRL_CROSS);
    key(e, SDL_SCANCODE_DOWN, false);
    e.cfg.keyboard_button_cross = input::PhysicalKeyCode::KeyX;
    e.cfg.keyboard_button_cross_alt = input::PhysicalKeyCode::Unbound;
    const SDL_Scancode minus[] = {SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_J, SDL_SCANCODE_I};
    const SDL_Scancode plus[] = {SDL_SCANCODE_D, SDL_SCANCODE_S, SDL_SCANCODE_L, SDL_SCANCODE_K};
    for (unsigned axis=0; axis<4; ++axis) {
        key(e, minus[axis], true); CHECK(e.ctrl.keyboard_state.axes[axis] == -1.f);
        key(e, plus[axis], true); CHECK(e.ctrl.keyboard_state.axes[axis] == 0.f);
        key(e, minus[axis], false); CHECK(e.ctrl.keyboard_state.axes[axis] == 1.f);
        key(e, plus[axis], false); CHECK(e.ctrl.keyboard_state.axes[axis] == 0.f);
    }
    key(e, SDL_SCANCODE_X, true);
    for (unsigned i=0; i<4; ++i) key(e, SDL_SCANCODE_X, true, true);
    buttons(e, SCE_CTRL_CROSS, SCE_CTRL_CROSS);
    focus = nullptr; pump(e); buttons(e, 0, 0);
    focus = reinterpret_cast<SDL_Window *>(uintptr_t(2)); pump(e); buttons(e, 0, 0);
    focus = test_window; pump(e); buttons(e, SCE_CTRL_CROSS, SCE_CTRL_CROSS);
    e.ime_active = true; pump(e); buttons(e, 0, 0);
    key(e, SDL_SCANCODE_X, false);
    e.ime_active = false; pump(e); buttons(e, 0, 0);
    held[SDL_SCANCODE_UNKNOWN] = true;
    held[SDL_SCANCODE_RSHIFT] = true;
    key_count = 1; pump(e); buttons(e, 0, 0);
    key_count = 0; pump(e); buttons(e, 0, 0);
    key_count = -1; pump(e); buttons(e, 0, 0);
    key_count = SDL_SCANCODE_COUNT;
    null_keyboard = true; pump(e); buttons(e, 0, 0); null_keyboard = false;
    held.fill(false);
    e.cfg.keyboard_button_r2 = input::PhysicalKeyCode::Unbound;
    e.cfg.keyboard_button_cross = input::PhysicalKeyCode::Fn;
    held[0x12] = true; pump(e); buttons(e, 0, 0);
    e.cfg.keyboard_button_cross = static_cast<input::PhysicalKeyCode>(0x0c0012);
    pump(e); buttons(e, 0, 0);
    e.cfg.keyboard_button_cross = input::PhysicalKeyCode::KeyX;
    held.fill(false); key(e, SDL_SCANCODE_X, true);
    pump(e, nullptr); buttons(e, 0, 0);
    key(e, SDL_SCANCODE_X, false);
    std::printf("%u checks pass: actual SDL event pump -> keyboard state -> controller reader; bindings, releases, axes, focus, IME and bounds\n", checks);
}
""".replace("@PUMP@", pump)
with tempfile.TemporaryDirectory(prefix="vita3k-sdl-keyboard-") as folder:
    temp = Path(folder)
    (temp / "test.cpp").write_text(prelude + updater + consumer + tests)
    includes = ["-I" + str(p) for p in (ROOT / "vita3k").glob("*/include")]
    includes += ["-I" + str(ROOT / p) for p in ("external/boost", "external/sdl/include", "external/fmt/include", "external/spdlog/include")]
    subprocess.run(["clang++", "-std=c++23", "-O1", "-g", "-fsanitize=address,undefined", "-DSPDLOG_FMT_EXTERNAL", *includes, str(temp / "test.cpp"), "-o", str(temp / "test")], check=True)
    subprocess.run([str(temp / "test")], check=True)
