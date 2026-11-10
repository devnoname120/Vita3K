from pathlib import Path
import re
import sys


def main() -> int:
    probe_source = Path(sys.argv[1]).read_text()
    integration_source = Path(sys.argv[2]).read_text()

    if "unsigned limit = 512" not in probe_source:
        raise AssertionError("bounded draw probe no longer has the 512-draw default")

    parser = re.search(
        r"inline unsigned bios_draw_probe_limit\(\).*?^\}",
        integration_source,
        re.MULTILINE | re.DOTALL,
    )
    if not parser:
        raise AssertionError("draw probe has no bounded configurable limit parser")
    body = parser.group(0)
    for required in (
        'std::getenv("VITA3K_BIOS_DRAW_PROBE_LIMIT")',
        "std::strtoul",
        "requested_limit > 0",
        "requested_limit <= 4096",
        "return 512",
    ):
        if required not in body:
            raise AssertionError(f"draw probe limit parser is missing {required}")

    if not re.search(
        r"\}\(\),\s*bios_draw_probe_limit\(\),\s*"
        r"bios_draw_probe_next_frame\(\)\s*\);",
        integration_source,
    ):
        raise AssertionError("draw probe ignores the configured bounded limit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
