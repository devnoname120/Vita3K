from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile


def require_integration_contract(integration: str) -> None:
    parser = re.search(
        r"inline bool bios_draw_probe_next_frame\(\).*?^\}",
        integration,
        re.MULTILINE | re.DOTALL,
    )
    if not parser:
        raise AssertionError("draw probe has no next-frame configuration parser")
    body = parser.group(0)
    for required in (
        'std::getenv("VITA3K_BIOS_DRAW_PROBE_NEXT_FRAME")',
        'std::strcmp(value, "1") == 0',
        "return false",
    ):
        if required not in body:
            raise AssertionError(f"next-frame parser is missing {required}")
    if not re.search(
        r"bios_draw_probe_limit\(\),\s*bios_draw_probe_next_frame\(\)\s*\);",
        integration,
    ):
        raise AssertionError("draw probe ignores the next-frame configuration")


def compile_and_run(header: Path) -> None:
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not compiler:
        raise AssertionError("no C++ compiler is available for the bounded probe behavior test")
    source = r'''
#include "bounded_draw_probe.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using renderer::vulkan::diagnostics::BoundedDrawProbe;

int main(int argc, char **argv) {
    if (argc != 2)
        return 10;
    const std::filesystem::path directory = argv[1];
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "request").put('\n');

    BoundedDrawProbe probe(directory, 2, true);
    const auto origin = BoundedDrawProbe::Clock::now();
    const auto writer = [](uint64_t frame) {
        return [frame](std::ostream &out, unsigned ordinal) {
            out << "FRAME " << frame << " ORDINAL " << ordinal << '\n';
        };
    };

    if (probe.visit(10, writer(10), origin) != BoundedDrawProbe::Result::idle)
        return 11;
    if (probe.visit(10, writer(10), origin + std::chrono::seconds(1)) != BoundedDrawProbe::Result::idle)
        return 12;
    if (probe.visit(11, writer(11), origin + std::chrono::seconds(2)) != BoundedDrawProbe::Result::recorded)
        return 13;
    if (probe.visit(11, writer(11), origin + std::chrono::seconds(3)) != BoundedDrawProbe::Result::completed)
        return 14;

    std::ifstream input(directory / "capture.log", std::ios::binary);
    const std::string capture((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input && capture.empty())
        return 15;
    if (capture.find("FRAME 10") != std::string::npos)
        return 16;
    if (capture.find("FRAME 11 ORDINAL 0") == std::string::npos
        || capture.find("FRAME 11 ORDINAL 1") == std::string::npos)
        return 17;
    if (capture.find("END draws=2 reason=draw_limit") == std::string::npos)
        return 18;

    const std::filesystem::path boundary_directory = directory / "boundary";
    std::filesystem::create_directories(boundary_directory);
    std::ofstream(boundary_directory / "request").put('\n');
    BoundedDrawProbe boundary_probe(boundary_directory, 10, true);
    if (boundary_probe.visit(20, writer(20), origin) != BoundedDrawProbe::Result::idle)
        return 19;
    if (boundary_probe.visit(21, writer(21), origin + std::chrono::seconds(1)) != BoundedDrawProbe::Result::recorded)
        return 20;
    if (boundary_probe.visit(21, writer(21), origin + std::chrono::seconds(2)) != BoundedDrawProbe::Result::recorded)
        return 21;
    if (boundary_probe.visit(22, writer(22), origin + std::chrono::seconds(3)) != BoundedDrawProbe::Result::completed)
        return 22;

    std::ifstream boundary_input(boundary_directory / "capture.log", std::ios::binary);
    const std::string boundary_capture((std::istreambuf_iterator<char>(boundary_input)), std::istreambuf_iterator<char>());
    if (boundary_capture.find("FRAME 20") != std::string::npos
        || boundary_capture.find("FRAME 22") != std::string::npos)
        return 23;
    if (boundary_capture.find("FRAME 21 ORDINAL 0") == std::string::npos
        || boundary_capture.find("FRAME 21 ORDINAL 1") == std::string::npos)
        return 24;
    if (boundary_capture.find("END draws=2 reason=frame_boundary") == std::string::npos)
        return 25;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="vita3k-next-frame-probe-") as temporary:
        temporary_path = Path(temporary)
        source_path = temporary_path / "test.cpp"
        binary_path = temporary_path / "test"
        capture_path = temporary_path / "capture"
        source_path.write_text(source)
        subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-pthread",
                "-I",
                str(header.parent),
                str(source_path),
                "-o",
                str(binary_path),
            ],
            check=True,
        )
        subprocess.run([str(binary_path), str(capture_path)], check=True)


def main() -> int:
    header = Path(sys.argv[1])
    integration = Path(sys.argv[2]).read_text()
    require_integration_contract(integration)
    compile_and_run(header)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
