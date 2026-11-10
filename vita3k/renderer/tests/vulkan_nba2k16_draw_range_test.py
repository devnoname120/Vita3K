from pathlib import Path
import re
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    helper = re.search(
        r"static bool nba2k16_skip_draw_range\(.*?^\}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not helper:
        raise AssertionError("NBA 2K16 draw-range diagnostic is missing")
    body = helper.group(0)
    for required in (
        '"NBA2K1601"',
        'std::getenv("VITA3K_NBA2K16_DRAW_CONTROL")',
        'std::getenv("VITA3K_NBA2K16_DRAW_RANGE")',
        'std::getenv("VITA3K_NBA2K16_KEEP_DRAW_RANGE")',
        'std::getenv("VITA3K_NBA2K16_DRAW_TRACE")',
        "std::filesystem::exists",
        "std::ifstream control(control_path)",
        "std::getline(control, control_range)",
        "std::from_chars",
        "context.scene_timestamp",
        "draw_index",
        "hex_string(vertex_program->renderer_data->hash)",
        "hex_string(fragment_program->renderer_data->hash)",
        '"VITA3K_NBA2K16_DRAW frame=%llu scene=%llu draw=%llu',
    ):
        if required not in body:
            raise AssertionError(f"NBA 2K16 draw-range diagnostic is missing {required}")
    control_check = body.index("std::filesystem::exists(control_path")
    trace_check = body.index('std::getenv("VITA3K_NBA2K16_DRAW_TRACE")')
    if control_check > trace_check:
        raise AssertionError("NBA 2K16 draw trace is not armed by the control file")

    if not re.search(
        r"scene_timestamp\s*!=\s*context\.scene_timestamp.*?"
        r"scene_timestamp\s*=\s*context\.scene_timestamp;.*?"
        r"draw_index\s*=\s*0;",
        body,
        re.DOTALL,
    ):
        raise AssertionError("NBA 2K16 draw ordinal is not reset per scene")
    if not re.search(r"\+\+draw_index", body):
        raise AssertionError("NBA 2K16 draw ordinal is not incremented")
    if not re.search(
        r"inside_selected_range\s*=\s*draw_index\s*>=\s*first_draw\s*&&\s*draw_index\s*<=\s*last_draw",
        body,
    ):
        raise AssertionError("NBA 2K16 draw selection is not bounded")
    if "keep_selected_range ? outside_selected_range : inside_selected_range" not in body:
        raise AssertionError("NBA 2K16 draw diagnostic cannot switch between keep and skip modes")

    for required in (
        'control_range.rfind("vertex:", 0) == 0',
        'control_range.rfind("fragment:", 0) == 0',
        'control_range.rfind("pair:", 0) == 0',
        'vertex_hash == control_range.substr(7)',
        'fragment_hash == control_range.substr(9)',
        'vertex_hash == control_range.substr(5, pair_separator - 5)',
        'fragment_hash == control_range.substr(pair_separator + 1)',
    ):
        if required not in body:
            raise AssertionError(f"NBA 2K16 exact shader selector is missing {required}")

    submit = source.index("context.render_cmd.drawIndexed(count, instance_count, 0, 0, 0);")
    gate = source.rfind("nba2k16_skip_draw_range", 0, submit)
    if gate < 0:
        raise AssertionError("NBA 2K16 draw-range diagnostic is not applied before submission")
    guarded = source[gate:submit]
    if not re.search(r"if\s*\(\s*!acxi_skip_hangar_environment\s*&&\s*!nba2k16_skip_draw\s*\)", guarded):
        raise AssertionError("NBA 2K16 draw-range diagnostic is not part of the indexed submission gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
