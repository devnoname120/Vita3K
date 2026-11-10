from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def compile_probe(header: Path, output: Path) -> None:
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not compiler:
        raise AssertionError("no C++ compiler is available")
    source = output.with_suffix(".cpp")
    source.write_text(
        r'''
#include "stream_ring_wrap_sync.h"
#include <iostream>

int main() {
    std::cout << renderer::vulkan::diagnostics::stream_ring_wrap_sync_enabled() << '\n';
    return 0;
}
'''
    )
    subprocess.run(
        [compiler, "-std=c++17", "-I", str(header.parent), str(source), "-o", str(output)],
        check=True,
    )


def run_probe(binary: Path, value: str | None) -> int:
    env = os.environ.copy()
    env.pop("VITA3K_STREAM_RING_SYNC_ON_WRAP", None)
    if value is not None:
        env["VITA3K_STREAM_RING_SYNC_ON_WRAP"] = value
    result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, check=True)
    return int(result.stdout.strip())


def main() -> int:
    root = Path(sys.argv[1])
    header = root / "vita3k/renderer/src/vulkan/stream_ring_wrap_sync.h"
    context = (root / "vita3k/renderer/src/vulkan/context.cpp").read_text()
    scene = (root / "vita3k/renderer/src/vulkan/scene.cpp").read_text()

    if not header.is_file():
        raise AssertionError("stream-ring wrap-sync helper is missing")

    with tempfile.TemporaryDirectory(prefix="vita3k-stream-ring-wrap-sync-") as temporary:
        binary = Path(temporary) / "probe"
        compile_probe(header, binary)
        scenarios = ((None, 0), ("0", 0), ("1", 1), ("true", 0), ("2", 0))
        for value, expected in scenarios:
            actual = run_probe(binary, value)
            if actual != expected:
                raise AssertionError(f"value={value!r}: expected {expected}, got {actual}")

    if "VITA3K_STREAM_RING_SYNC_EACH_FRAME" in context:
        raise AssertionError("invalid per-frame ring synchronization remains in new_frame")
    for fragment in (
        "context.vertex_stream_ring_buffer.reset()",
        "context.index_stream_ring_buffer.reset()",
        "context.vertex_uniform_stream_ring_buffer.reset()",
        "context.fragment_uniform_stream_ring_buffer.reset()",
        "context.vertex_info_uniform_buffer.reset()",
        "context.fragment_info_uniform_buffer.reset()",
    ):
        if fragment in context:
            raise AssertionError(f"invalid frame-boundary reset remains: {fragment}")

    required = (
        '#include "stream_ring_wrap_sync.h"',
        "diagnostics::stream_ring_wrap_sync_enabled()",
        "context.stop_recording(empty_notification, empty_notification, true)",
        "context.state.device.waitIdle()",
        "VITA3K_STREAM_RING_WRAP_SYNC",
        "context.vertex_stream_ring_buffer.will_wrap",
        "context.index_stream_ring_buffer.will_wrap",
    )
    for fragment in required:
        if fragment not in scene:
            raise AssertionError(f"wrap-only synchronization is missing: {fragment}")

    draw = scene.find("void draw(VKContext &context")
    sync = scene.find("synchronize_stream_ring_wraps(context,", draw)
    frame_setup = scene.find("context.check_for_macroblock_change(true);", draw)
    index_allocate = scene.find("context.index_stream_ring_buffer.allocate", frame_setup)
    vertex_bind = scene.find("bind_vertex_streams(context,", index_allocate)
    if min(draw, sync, frame_setup, index_allocate, vertex_bind) < 0:
        raise AssertionError("could not locate wrap synchronization ordering")
    if not sync < frame_setup < index_allocate < vertex_bind:
        raise AssertionError("wrap synchronization must happen before render-pass setup and ring allocation")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
