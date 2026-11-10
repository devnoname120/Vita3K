from pathlib import Path
import sys


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    gate = function_body(source, "static bool bloonstd5hd_retry_first_textured_draw()")
    for required in (
        "#ifdef VITA3K_ENABLE_TTBR_REMAP_HLE",
        'std::getenv("VITA3K_TTBR_REMAP_HLE_TITLE")',
        'std::strcmp(title, "BIOS00001") == 0',
    ):
        if required not in gate:
            raise AssertionError(f"Bloons draw retry gate is missing {required}")

    draw = function_body(source, "void draw(VKContext &context")
    for required in (
        "const uint16_t fragment_texture_count",
        "context.last_frag_texture_count == 0",
        "fragment_texture_count > 0",
        "count == 6",
        "instance_count == 1",
        "const bool bloonstd5hd_retry_draw",
    ):
        if required not in draw:
            raise AssertionError(f"Bloons draw retry is missing {required}")
    if draw.count("draw_bind_descriptors(context, mem);") != 1:
        raise AssertionError("Bloons draw retry must preserve stock descriptor binding")
    if "bloonstd5hd_rebind_pipeline" in draw:
        raise AssertionError("Bloons draw retry must not alter pipeline binding")
    draw_call = "context.render_cmd.drawIndexed(count, instance_count, 0, 0, 0);"
    if draw.count(draw_call) != 2:
        raise AssertionError("Bloons draw retry must issue exactly one conditional duplicate draw")
    first = draw.index(draw_call)
    guard = draw.index("if (bloonstd5hd_retry_draw)", first)
    second = draw.index(draw_call, first + 1)
    if not first < guard < second:
        raise AssertionError("Bloons draw retry order is incorrect")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
