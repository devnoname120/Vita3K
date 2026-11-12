from pathlib import Path
import re
import sys


def main() -> int:
    batch = Path(sys.argv[1]).read_text()
    state = Path(sys.argv[2]).read_text()

    creator = re.search(
        r"^static SDL_Thread \*create_render_thread\(.*?^\}",
        batch,
        re.MULTILINE | re.DOTALL,
    )
    if not creator:
        raise AssertionError("renderer thread factory is missing")

    body = creator.group(0)
    for required in (
        '#ifdef VITA3K_ENABLE_TTBR_REMAP_HLE',
        'std::getenv("VITA3K_TTBR_REMAP_HLE_TITLE")',
        'SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER',
        'SDL_PROP_THREAD_CREATE_NAME_STRING',
        'SDL_PROP_THREAD_CREATE_USERDATA_POINTER',
        'SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER',
        '32 * 1024 * 1024',
        'SDL_CreateThreadWithProperties(properties)',
        'SDL_CreateThread(&render_thread_entry, "Vita3K Renderer", arguments)',
    ):
        if required not in body:
            raise AssertionError(f"renderer thread factory is missing {required}")

    if body.index('std::getenv("VITA3K_TTBR_REMAP_HLE_TITLE")') > body.index(
        'SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER'
    ):
        raise AssertionError("renderer stack override is not title-scoped")
    if "ASPHALT07" in body or "8 * 1024 * 1024" in body:
        raise AssertionError("renderer stack size must not depend on the title")
    if "SDL_Thread *render_thread = nullptr;" not in state:
        raise AssertionError("renderer state does not own the SDL thread")
    if "SDL_WaitThread(state.render_thread, nullptr);" not in batch:
        raise AssertionError("renderer shutdown does not join the SDL thread")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
