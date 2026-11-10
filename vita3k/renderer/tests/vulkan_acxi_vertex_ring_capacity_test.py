from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


DEFAULT_BYTES = 64 * 1024 * 1024
DIAGNOSTIC_BYTES = 512 * 1024 * 1024


def compile_probe(header: Path, output: Path) -> None:
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not compiler:
        raise AssertionError("no C++ compiler is available for the ring-capacity behavior test")
    source = output.with_suffix(".cpp")
    source.write_text(
        r'''
#include "acxi_ring_capacity.h"

#include <iostream>

int main() {
    std::cout << renderer::vulkan::diagnostics::acxi_vertex_stream_ring_capacity_bytes() << '\n';
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


def run_probe(
    binary: Path,
    title: str | None,
    capacity: str | None,
    generic_capacity: str | None,
) -> int:
    env = os.environ.copy()
    for name in (
        "VITA3K_TTBR_REMAP_HLE_TITLE",
        "VITA3K_ACXI_VERTEX_STREAM_RING_MIB",
        "VITA3K_VERTEX_STREAM_RING_MIB",
    ):
        env.pop(name, None)
    if title is not None:
        env["VITA3K_TTBR_REMAP_HLE_TITLE"] = title
    if capacity is not None:
        env["VITA3K_ACXI_VERTEX_STREAM_RING_MIB"] = capacity
    if generic_capacity is not None:
        env["VITA3K_VERTEX_STREAM_RING_MIB"] = generic_capacity
    result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, check=True)
    return int(result.stdout.strip())


def main() -> int:
    root = Path(sys.argv[1])
    header = root / "vita3k/renderer/src/vulkan/acxi_ring_capacity.h"
    creation = (root / "vita3k/renderer/src/vulkan/creation.cpp").read_text()
    if not header.is_file():
        raise AssertionError("ACXI vertex-ring capacity helper is missing")
    required = (
        '#include "acxi_ring_capacity.h"',
        "diagnostics::acxi_vertex_stream_ring_capacity_bytes()",
    )
    for fragment in required:
        if fragment not in creation:
            raise AssertionError(f"Vulkan context creation is missing {fragment}")

    with tempfile.TemporaryDirectory(prefix="vita3k-acxi-ring-capacity-") as temporary:
        binary = Path(temporary) / "probe"
        compile_probe(header, binary)
        scenarios = (
            (None, None, None, DEFAULT_BYTES),
            ("OTHER0001", "512", None, DEFAULT_BYTES),
            ("ACXI00001", None, None, DEFAULT_BYTES),
            ("ACXI00001", "512", None, DIAGNOSTIC_BYTES),
            ("ACXI00001", "63", None, DEFAULT_BYTES),
            ("ACXI00001", "513", None, DEFAULT_BYTES),
            ("ACXI00001", "512x", None, DEFAULT_BYTES),
            (None, None, "128", 128 * 1024 * 1024),
            ("OTHER0001", "512", "256", 256 * 1024 * 1024),
            (None, None, "63", DEFAULT_BYTES),
            (None, None, "513", DEFAULT_BYTES),
            (None, None, "128x", DEFAULT_BYTES),
        )
        for title, capacity, generic_capacity, expected in scenarios:
            actual = run_probe(binary, title, capacity, generic_capacity)
            if actual != expected:
                raise AssertionError(
                    f"title={title!r} capacity={capacity!r} generic={generic_capacity!r}: "
                    f"expected {expected}, got {actual}"
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
