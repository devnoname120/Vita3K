from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


DEFAULT_BYTES = 64 * 1024 * 1024


def compile_probe(header: Path, output: Path) -> None:
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not compiler:
        raise AssertionError("no C++ compiler is available for the ring-capacity behavior test")
    source = output.with_suffix(".cpp")
    source.write_text(
        r'''
#include "vertex_stream_ring_capacity.h"

#include <iostream>

int main() {
    std::cout << renderer::vulkan::diagnostics::vertex_stream_ring_capacity_bytes() << '\n';
    return 0;
}
'''
    )
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-I",
            str(header.parent),
            str(source),
            "-o",
            str(output),
        ],
        check=True,
    )


def run_probe(binary: Path, generic_capacity: str | None) -> int:
    env = os.environ.copy()
    env.pop("VITA3K_VERTEX_STREAM_RING_MIB", None)
    if generic_capacity is not None:
        env["VITA3K_VERTEX_STREAM_RING_MIB"] = generic_capacity
    result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, check=True)
    return int(result.stdout.strip())


def main() -> int:
    root = Path(sys.argv[1])
    header = root / "vita3k/renderer/src/vulkan/vertex_stream_ring_capacity.h"
    creation = (root / "vita3k/renderer/src/vulkan/creation.cpp").read_text()
    if not header.is_file():
        raise AssertionError("vertex-ring capacity helper is missing")
    required = (
        '#include "vertex_stream_ring_capacity.h"',
        "diagnostics::vertex_stream_ring_capacity_bytes()",
    )
    for fragment in required:
        if fragment not in creation:
            raise AssertionError(f"Vulkan context creation is missing {fragment}")

    with tempfile.TemporaryDirectory(prefix="vita3k-ring-capacity-") as temporary:
        binary = Path(temporary) / "probe"
        compile_probe(header, binary)
        scenarios = (
            (None, DEFAULT_BYTES),
            ("128", 128 * 1024 * 1024),
            ("512", 512 * 1024 * 1024),
            ("63", DEFAULT_BYTES),
            ("513", DEFAULT_BYTES),
            ("128x", DEFAULT_BYTES),
        )
        for generic_capacity, expected in scenarios:
            actual = run_probe(binary, generic_capacity)
            if actual != expected:
                raise AssertionError(
                    f"generic={generic_capacity!r}: expected {expected}, got {actual}"
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
