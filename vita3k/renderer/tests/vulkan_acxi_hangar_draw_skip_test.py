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
        raise AssertionError("ACXI hangar draw-skip diagnostic is missing")
    body = helper.group(0)
    for required in (
        '"ACXI00001"',
        'std::getenv("VITA3K_ACXI_SKIP_VERTEX_HASH")',
        'std::getenv("VITA3K_ACXI_SKIP_TRIGGER")',
        'std::getenv("VITA3K_ACXI_SKIP_DRAW_RANGE")',
        "std::filesystem::exists",
        "std::ifstream control(trigger)",
        "std::getline(control, control_range)",
        "hex_string(vertex_program->renderer_data->hash)",
        "matching_scene_timestamp",
        "matching_draw_index",
        "std::from_chars",
    ):
        if required not in body:
            raise AssertionError(f"ACXI hangar draw-skip diagnostic is missing {required}")
    if "matching_run_active" in body:
        raise AssertionError("ACXI hangar draw index is still scoped to contiguous shader runs")
    if not re.search(
        r"matching_scene_timestamp\s*!=\s*context\.scene_timestamp.*?"
        r"matching_scene_timestamp\s*=\s*context\.scene_timestamp;.*?"
        r"matching_draw_index\s*=\s*0;",
        body,
        re.DOTALL,
    ):
        raise AssertionError("ACXI hangar draw index is not reset once per scene timestamp")
    if not re.search(r"if\s*\(\s*!matches_environment\s*\)\s*return\s+false;", body):
        raise AssertionError("Nonmatching shaders do not preserve the scene-global ACXI draw ordinal")
    if not re.search(
        r"matching_draw_index\s*>=\s*first_draw\s*&&\s*matching_draw_index\s*<=\s*last_draw",
        body,
    ):
        raise AssertionError("ACXI hangar draw skip is not bounded to the requested contiguous draw range")

    submit = source.index("context.render_cmd.drawIndexed(count, instance_count, 0, 0, 0);")
    gate = source.rfind("acxi_skip_hangar_environment_draw", 0, submit)
    if gate < 0:
        raise AssertionError("ACXI hangar draw skip is not applied before indexed submission")
    guarded = source[gate:submit]
    if not re.search(
        r"if\s*\(\s*!acxi_skip_hangar_environment\s*&&\s*!nba2k16_skip_draw\s*\)\s*$",
        guarded,
        re.MULTILINE,
    ):
        raise AssertionError("ACXI hangar draw skip is not preserved in the indexed submission gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
