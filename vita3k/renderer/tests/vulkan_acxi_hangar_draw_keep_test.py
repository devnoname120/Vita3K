from pathlib import Path
import re
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    helper = re.search(
        r"static bool acxi_skip_hangar_environment_draw\(.*?^\}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not helper:
        raise AssertionError("ACXI hangar range diagnostic is missing")
    body = helper.group(0)
    for required in (
        'std::getenv("VITA3K_ACXI_KEEP_DRAW_RANGE")',
        "const bool keep_selected_range",
        "matching_draw_index < first_draw || matching_draw_index > last_draw",
        "keep_selected_range ? outside_selected_range : inside_selected_range",
    ):
        if required not in body:
            raise AssertionError(f"ACXI hangar keep-range diagnostic is missing {required}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
