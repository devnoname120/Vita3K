from pathlib import Path
import re
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    block = re.search(
        r'const char \*capture_setting = std::getenv\("VITA3K_WAW_CAPTURE"\);.*?unsigned captured_frames = 0;',
        source,
        re.DOTALL,
    )
    if not block:
        raise AssertionError("SDL frontend has no bounded automatic capture policy")

    body = block.group(0)
    for required in (
        'std::getenv("VITA3K_WAW_CAPTURE_LIMIT")',
        "std::strtoul(capture_limit_setting, &capture_limit_end, 10)",
        "capture_limit_end != capture_limit_setting",
        "*capture_limit_end == '\\0'",
        "requested_capture_limit > 0",
        "requested_capture_limit <= 3600",
        "unsigned capture_limit = 24",
    ):
        if required not in body:
            raise AssertionError(f"automatic capture policy is missing {required}")

    loop = source[source.index("while (running) {") :]
    if "captured_frames < capture_limit" not in loop:
        raise AssertionError("automatic capture loop ignores the configured capture limit")
    if "captured_frames < 24" in loop:
        raise AssertionError("automatic capture loop still has the fixed 24-frame cap")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
