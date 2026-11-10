from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


TARGET_HASH = "6ec1c320e585ac486eb37b911ded7d8842d0a012aa981abd2d3e8ad2e224b577"


def compile_probe(header: Path, output: Path) -> None:
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not compiler:
        raise AssertionError("no C++ compiler is available for the crowd-uniform trace behavior test")
    source = output.with_suffix(".cpp")
    source.write_text(
        r'''
#include "nba2k16_crowd_uniform_trace.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    std::array<uint8_t, 32> hash{};
    const char *text = argv[1];
    for (size_t index = 0; index < hash.size(); ++index) {
        const auto high = renderer::vulkan::diagnostics::nba2k16_crowd_hex_nibble(text[index * 2]);
        const auto low = renderer::vulkan::diagnostics::nba2k16_crowd_hex_nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0)
            return 3;
        hash[index] = static_cast<uint8_t>((high << 4) | low);
    }
    std::array<uint8_t, renderer::vulkan::diagnostics::NBA2K16_CROWD_UNIFORM_BYTES> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(index * 17u + 3u);
    const auto hashes = renderer::vulkan::diagnostics::nba2k16_crowd_uniform_hashes(bytes.data(), bytes.size());
    std::cout
        << renderer::vulkan::diagnostics::nba2k16_crowd_uniform_trace_enabled(hash.data(), hash.size()) << ' '
        << renderer::vulkan::diagnostics::NBA2K16_CROWD_UNIFORM_TRACE_LIMIT << ' '
        << renderer::vulkan::diagnostics::NBA2K16_CROWD_UNIFORM_BYTES << ' '
        << renderer::vulkan::diagnostics::NBA2K16_CROWD_INSTANCE_OFFSET << ' '
        << renderer::vulkan::diagnostics::NBA2K16_CROWD_INSTANCE_BYTES << ' '
        << renderer::vulkan::diagnostics::NBA2K16_CROWD_MATRIX_OFFSET << ' '
        << renderer::vulkan::diagnostics::NBA2K16_CROWD_MATRIX_BYTES << ' '
        << std::hex << hashes.full << ' ' << hashes.instance << ' ' << hashes.matrix << '\n';
    return 0;
}
'''
    )
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(header.parent),
            str(source),
            "-o",
            str(output),
        ],
        check=True,
    )


def run_probe(binary: Path, title: str | None, enabled: str | None, shader_hash: str) -> list[str]:
    environment = os.environ.copy()
    environment.pop("VITA3K_TTBR_REMAP_HLE_TITLE", None)
    environment.pop("VITA3K_NBA2K16_CROWD_UNIFORM_TRACE", None)
    if title is not None:
        environment["VITA3K_TTBR_REMAP_HLE_TITLE"] = title
    if enabled is not None:
        environment["VITA3K_NBA2K16_CROWD_UNIFORM_TRACE"] = enabled
    result = subprocess.run(
        [str(binary), shader_hash],
        env=environment,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip().split()


def main() -> int:
    root = Path(sys.argv[1])
    header = root / "vita3k/renderer/src/vulkan/nba2k16_crowd_uniform_trace.h"
    scene = (root / "vita3k/renderer/src/vulkan/scene.cpp").read_text()
    if not header.is_file():
        raise AssertionError("NBA 2K16 crowd-uniform trace helper is missing")

    required_scene_markers = (
        '#include "nba2k16_crowd_uniform_trace.h"',
        "nba2k16_crowd_uniform_trace_enabled(program->hash.data(), program->hash.size())",
        "VITA3K_NBA2K16_CROWD_UB_UPLOAD",
        "VITA3K_NBA2K16_CROWD_UB_DRAW",
        "source_hashes.instance",
        "source_hashes.matrix",
        "ring_hashes.instance",
        "ring_hashes.matrix",
        "draw_bind_descriptors(context, mem);",
    )
    for marker in required_scene_markers:
        if marker not in scene:
            raise AssertionError(f"Vulkan scene crowd-uniform probe is missing {marker}")
    bind = scene.index("draw_bind_descriptors(context, mem);")
    draw_trace = scene.index("VITA3K_NBA2K16_CROWD_UB_DRAW", bind)
    indexed_draw = scene.index("context.render_cmd.drawIndexed", draw_trace)
    if not bind < draw_trace < indexed_draw:
        raise AssertionError("crowd-uniform draw probe must observe the bound ring before the indexed draw")

    with tempfile.TemporaryDirectory(prefix="vita3k-nba2k16-crowd-uniform-") as temporary:
        binary = Path(temporary) / "probe"
        compile_probe(header, binary)
        scenarios = (
            (None, None, TARGET_HASH, "0"),
            ("OTHER0001", "1", TARGET_HASH, "0"),
            ("NBA2K1601", None, TARGET_HASH, "0"),
            ("NBA2K1601", "true", TARGET_HASH, "0"),
            ("NBA2K1601", "1", "00" * 32, "0"),
            ("NBA2K1601", "1", TARGET_HASH, "1"),
        )
        reference = None
        for title, enabled, shader_hash, expected_enabled in scenarios:
            values = run_probe(binary, title, enabled, shader_hash)
            if values[0] != expected_enabled:
                raise AssertionError(
                    f"title={title!r} enabled={enabled!r} hash={shader_hash}: "
                    f"expected gate {expected_enabled}, got {values[0]}"
                )
            constants = tuple(map(int, values[1:7]))
            if constants != (16, 1616, 64, 400, 464, 1152):
                raise AssertionError(f"unexpected crowd-uniform layout: {constants}")
            hashes = tuple(values[7:10])
            if reference is None:
                reference = hashes
            elif hashes != reference:
                raise AssertionError("crowd-uniform hashes are not deterministic")
        if reference is None or any(value == "0" for value in reference):
            raise AssertionError(f"crowd-uniform segment hashes are invalid: {reference}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
