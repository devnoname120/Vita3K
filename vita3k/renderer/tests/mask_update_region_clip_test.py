from pathlib import Path
import re
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    handler = re.search(
        r"^COMMAND_SET_STATE\(region_clip\) \{.*?^\}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not handler:
        raise AssertionError("renderer region-clip state handler is missing")

    body = handler.group(0)
    for required in (
        "render_context->record.is_maskupdate",
        "renderer.current_backend == Backend::Vulkan",
        "region_clip_min.x = static_cast<SceInt>(xMin)",
        "region_clip_min.y = static_cast<SceInt>(yMin)",
        "region_clip_max.x = static_cast<SceInt>(xMax)",
        "region_clip_max.y = static_cast<SceInt>(yMax)",
        "align_down(xMin, SCE_GXM_TILE_SIZEX)",
        "align_down(yMin, SCE_GXM_TILE_SIZEY)",
        "align(xMax, SCE_GXM_TILE_SIZEX)",
        "align(yMax, SCE_GXM_TILE_SIZEY)",
    ):
        if required not in body:
            raise AssertionError(f"mask-update region clipping is missing {required}")

    if body.index("render_context->record.is_maskupdate") > body.index("align_down(xMin"):
        raise AssertionError("mask-update detection happens after tile alignment")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
