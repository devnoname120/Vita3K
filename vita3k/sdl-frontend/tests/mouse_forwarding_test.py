from pathlib import Path
import re
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    helper = re.search(
        r"^void update_mouse_input\(EmuEnvState &emuenv, SDL_Window \*window\) \{.*?^\}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not helper:
        raise AssertionError("SDL frontend has no mouse-to-touch updater")

    body = helper.group(0)
    for required in (
        "SDL_GetMouseState",
        "SDL_GetWindowSize(window",
        "SDL_GetWindowSizeInPixels(window",
        "SDL_GetKeyboardFocus() == window",
        "SDL_GetMouseFocus() == window",
        "touch.finger_count > 0",
        "window_x * scale_x",
        "window_y * scale_y",
        "touch.mouse_x",
        "touch.mouse_y",
        "touch.mouse_button_left",
        "touch.mouse_button_right",
        "touch.renderer_focused",
        "SDL_BUTTON_LMASK",
        "SDL_BUTTON_RMASK",
    ):
        if required not in body:
            raise AssertionError(f"mouse-to-touch updater is missing {required}")

    if 'if (touch.finger_count > 0 && !(test_touch_path && *test_touch_path))' not in body:
        raise AssertionError("synthetic touch must override stale SDL finger state")

    if body.index('const char *test_touch_path = std::getenv("VITA3K_TEST_TOUCH_FILE")') > body.index('if (!window)'):
        raise AssertionError("synthetic touch arming must be observable before the null-window guard")
    if 'Test front touch armed from {} window={} finger_count={}' not in body:
        raise AssertionError("synthetic touch arming must have one bounded diagnostic")

    if 'test_touch.phase == 3 && !trigger_exists' not in body:
        raise AssertionError("completed synthetic touch must re-arm after trigger removal")
    for required in (
        "uint64_t generation = 0;",
        "uint64_t trigger_generation = 0;",
        "generation_probe >> trigger_x >> trigger_y >> trigger_hold_ms >> trigger_generation",
        "trigger_generation != test_touch.generation",
        "uint64_t requested_generation = test_touch.generation + 1;",
        "trigger >> requested_generation",
        "test_touch.generation = requested_generation",
    ):
        if required not in body:
            raise AssertionError(f"generation-aware synthetic touch is missing {required}")

    for required in (
        "std::ifstream trigger(test_touch.path)",
        "trigger >> test_touch.normalized_x >> test_touch.normalized_y",
        "test_touch.normalized_x < 0.f || test_touch.normalized_x > 1.f",
        "test_touch.normalized_y < 0.f || test_touch.normalized_y > 1.f",
        "window_width) * test_touch.normalized_x",
        "window_height) * test_touch.normalized_y",
    ):
        if required not in body:
            raise AssertionError(f"coordinate-aware synthetic touch is missing {required}")

    for required in (
        "unsigned hold_ms = 250;",
        "trigger >> requested_hold_ms",
        "requested_hold_ms <= 10000",
        "test_touch.release_tick = SDL_GetTicks() + hold_ms",
        "test_touch.phase == 1 && std::filesystem::exists(test_touch.path)",
        "latest >> latest_x >> latest_y",
        "test_touch.normalized_x = latest_x",
        "test_touch.normalized_y = latest_y",
    ):
        if required not in body:
            raise AssertionError(f"drag-capable synthetic touch is missing {required}")

    for required in (
        "struct ScriptedInputState",
        'std::getenv("VITA3K_SCRIPTED_INPUT")',
        "state.consumed_bytes",
        'LOG_INFO("Scripted input size={} consumed={}"',
        'command == "touch_down"',
        'command == "touch_move"',
        'command == "touch_up"',
        "handle_touch_event(emuenv.touch, finger)",
        'command == "controller"',
        'command == "controller_clear"',
        "const auto apply_controller_state = [&]()",
        "if (file_size == state.consumed_bytes) {",
        "apply_controller_state();",
        'LOG_INFO("Scripted touch {} x={} y={}"',
    ):
        if required not in source:
            raise AssertionError(f"append-only scripted input is missing {required}")

    no_change = source[source.index("if (file_size == state.consumed_bytes) {") :]
    no_change = no_change[: no_change.index("}") + 1]
    if no_change.index("apply_controller_state();") > no_change.index("return true;"):
        raise AssertionError("held scripted controller state is not reapplied before the unchanged-file return")

    loop = source[source.index("while (running) {") :]
    if "apply_scripted_input(emuenv, scripted_input" not in loop:
        raise AssertionError("SDL main loop does not poll append-only scripted input")
    if "update_mouse_input(emuenv, window);" not in loop:
        raise AssertionError("SDL main loop does not poll mouse state")
    if "update_mouse_input(emuenv, nullptr);" not in source:
        raise AssertionError("SDL cleanup does not release mouse touch state")

    ime_helper = re.search(
        r"^void update_test_ime\(EmuEnvState &emuenv, const Hooks &hooks\) \{.*?^\}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not ime_helper:
        raise AssertionError("SDL frontend has no file-driven test IME updater")
    ime_body = ime_helper.group(0)
    for required in (
        'std::getenv("VITA3K_TEST_IME_FILE")',
        "std::filesystem::exists(test_ime.path)",
        "is_any_ime_active(emuenv)",
        "std::getline(trigger, text)",
        "text.size() > 2048",
        "const std::lock_guard<std::mutex> lock(emuenv.ime.mutex)",
        "emuenv.ime.str.clear()",
        "emuenv.ime.edit_text = {}",
        "ime_commit_text(emuenv.ime, string_utils::utf8_to_utf16(text))",
        "notify_ime_changed(hooks)",
        "submit_current_ime(emuenv)",
        "test_ime.consumed = true",
    ):
        if required not in ime_body:
            raise AssertionError(f"file-driven test IME updater is missing {required}")
    if "update_test_ime(emuenv, hooks);" not in loop:
        raise AssertionError("SDL main loop does not poll the test IME trigger")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
