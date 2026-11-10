from pathlib import Path
import re
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    start = source.index("void draw(VKContext &context")
    end = source.index("\n}\n\n} // namespace renderer::vulkan", start)
    body = source[start:end]

    guard = re.search(
        r"if\s*\(\s*!context\.state\.features\.use_mask_bit\s*&&\s*"
        r"gxm_fragment_program\.is_maskupdate\s*\)\s*\{\s*return;\s*\}",
        body,
        re.DOTALL,
    )
    if not guard:
        raise AssertionError("Vulkan draw does not skip mask-update programs when mask-bit emulation is disabled")

    first_render_work = min(
        body.index("context.check_for_macroblock_change(true)"),
        body.index("context.start_render_pass()"),
    )
    if guard.start() > first_render_work:
        raise AssertionError("mask-update skip happens after Vulkan render work begins")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
