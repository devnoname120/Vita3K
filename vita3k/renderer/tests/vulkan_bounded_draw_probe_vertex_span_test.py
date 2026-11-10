from pathlib import Path
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    for required in (
        "uint32_t min_index",
        "min_index = std::min<uint32_t>(min_index, values[i])",
        'first={} min={} max={}',
        "uint64_t(min_index) * stream.stride",
        "((uint64_t(max_index) - min_index) + 1u) * stream.stride",
        "std::min({ remaining, requested_bytes, vertex_budget })",
    ):
        if required not in source:
            raise AssertionError(f"draw probe does not preserve the full referenced vertex span: {required}")
    if "unit_size * (constant ? 1u : 6u)" in source:
        raise AssertionError("draw probe still truncates each vertex stream to six records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
