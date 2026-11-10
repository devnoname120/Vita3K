from pathlib import Path
import re
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
                return source[brace + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    root = Path(sys.argv[1])
    scene = (root / "vita3k/renderer/src/vulkan/scene.cpp").read_text()
    recompiler = (root / "vita3k/shader/src/spirv_recompiler.cpp").read_text()

    control = function_body(scene, "static int acxi_force_clip_plane")
    for required in (
        'std::getenv("VITA3K_ACXI_FORCE_CLIP_PLANE_FILE")',
        'std::getenv("VITA3K_ACXI_FORCE_CLIP_VERTEX_HASH")',
        'config.run_app_path != "ACXI00001"',
        "hex_string(vertex_hash) != expected_hash",
        "std::ifstream control(control_path)",
        "std::from_chars",
        "plane < 0 || plane > 5",
        "return plane",
    ):
        if required not in control:
            raise AssertionError(f"ACXI per-plane control is missing {required}")

    draw = function_body(scene, "void draw(VKContext &context")
    for required in (
        "const int force_clip_plane = acxi_force_clip_plane(config, vert_render_data->hash)",
        "force_clip_plane >= 0 ? 3.0f + static_cast<float>(force_clip_plane)",
        "force_clip_reject ? 2.0f",
        "gl_clip_compat ? 1.0f : 0.0f",
    ):
        if required not in draw:
            raise AssertionError(f"Vulkan draw path is missing {required}")

    finalize = function_body(recompiler, "static spv::Function *make_vert_finalize_function")
    for required in (
        "spv::OpFOrdEqual",
        "force_clip_reject_pred",
        "force_clip_plane_pred",
        "3.0f + static_cast<float>(index)",
        "spv::OpLogicalOr",
        "force_clip_pred, neg_one, selected_clip_distance",
    ):
        if required not in finalize:
            raise AssertionError(f"SPIR-V per-plane diagnostic is missing {required}")
    if not re.search(
        r"force_clip_reject_pred\s*=\s*b\.createOp\(\s*"
        r"spv::OpFOrdEqual.*?post_viewport_clip.*?two",
        finalize,
        re.DOTALL,
    ):
        raise AssertionError("force-all mode is not isolated from per-plane diagnostic values")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
