from pathlib import Path
import re
import sys


def main() -> int:
    root = Path(sys.argv[1])
    uniform_block = (root / "vita3k/shader/include/shader/uniform_block.h").read_text()
    version_header = (root / "vita3k/shader/include/shader/spirv_recompiler.h").read_text()
    shaders = (root / "vita3k/renderer/src/shaders.cpp").read_text()
    pipeline_cache = (root / "vita3k/renderer/src/vulkan/pipeline_cache.cpp").read_text()

    render_block = re.search(
        r"struct RenderVertUniformBlock\s*\{(?P<body>.*?)\};",
        uniform_block,
        re.DOTALL,
    )
    if not render_block:
        raise AssertionError("RenderVertUniformBlock is missing")

    fields = (
        "viewport_flag",
        "w_clamp_enabled",
        "w_clamp_value",
        "post_viewport_clip",
        "screen_width",
        "screen_height",
        "z_offset",
        "z_scale",
    )
    body = render_block.group("body")
    offsets = [body.find(field) for field in fields]
    if any(offset < 0 for offset in offsets) or offsets != sorted(offsets):
        raise AssertionError("expanded vertex-render uniform ABI has an unexpected field order")

    version_match = re.search(
        r"CURRENT_VERSION\s*=\s*(?P<version>\d+)\s*;",
        version_header,
    )
    if not version_match:
        raise AssertionError("shader cache version is missing")
    if int(version_match.group("version")) < 14:
        raise AssertionError("expanded vertex-render uniform ABI must invalidate vk13 shader caches")

    for required in (
        "versionInFile != shader::CURRENT_VERSION",
        "const uint32_t versionInFile = shader::CURRENT_VERSION",
    ):
        if required not in shaders:
            raise AssertionError(f"shader hash cache is missing {required}")

    for required in (
        'fmt::format("pipeline-cache-vk{}.dat", shader::CURRENT_VERSION)',
        'fmt::format("vk{}", shader::CURRENT_VERSION)',
        'fmt::format("vk{}-{}.spv", shader::CURRENT_VERSION, hex_string(shader_hash))',
    ):
        if required not in pipeline_cache:
            raise AssertionError(f"Vulkan shader cache is missing {required}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
