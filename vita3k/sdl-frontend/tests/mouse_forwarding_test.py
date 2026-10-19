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

    loop = source[source.index("while (running) {") :]
    if "update_mouse_input(emuenv, window);" not in loop:
        raise AssertionError("SDL main loop does not poll mouse state")
    if "update_mouse_input(emuenv, nullptr);" not in source:
        raise AssertionError("SDL cleanup does not release mouse touch state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
