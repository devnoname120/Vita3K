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
    renderer = (root / "vita3k/renderer/src/vulkan/renderer.cpp").read_text()
    scene = (root / "vita3k/renderer/src/vulkan/scene.cpp").read_text()
    gl_draw = (root / "vita3k/renderer/src/gl/draw.cpp").read_text()
    recompiler = (root / "vita3k/shader/src/spirv_recompiler.cpp").read_text()

    enabled = re.search(
        r"vk::PhysicalDeviceFeatures enabled_features\s*\{(?P<body>.*?)\n\s*\};",
        renderer,
        re.DOTALL,
    )
    if not enabled or ".shaderClipDistance = physical_device_features.shaderClipDistance" not in enabled.group("body"):
        raise AssertionError("Vulkan device creation does not enable supported shader clip distances")

    body = function_body(
        recompiler,
        "static spv::Function *make_vert_finalize_function",
    )
    for required in (
        "spv::CapabilityClipDistance",
        "spv::BuiltInClipDistance",
        '"gl_ClipDistance"',
        "b.makeArrayType(f32, b.makeUintConstant(6), 0)",
        "translation_state.interfaces.push_back(clip_distance_var)",
        "const spv::Id clip_x_min = b.createBinOp(spv::OpFAdd, f32, original_x, original_w)",
        "const spv::Id clip_x_max = b.createBinOp(spv::OpFSub, f32, original_w, original_x)",
        "const spv::Id clip_y_min = b.createBinOp(spv::OpFAdd, f32, original_y, original_w)",
        "const spv::Id clip_y_max = b.createBinOp(spv::OpFSub, f32, original_w, original_y)",
        "const spv::Id clip_z_min = b.createBinOp(spv::OpFAdd, f32, original_z, original_w)",
        "const spv::Id clip_z_max = b.createBinOp(spv::OpFSub, f32, original_w, original_z)",
        "post_viewport_clip_pred",
        "const spv::Id force_clip_reject_pred",
        "b.createOp(spv::OpSelect, f32, { post_viewport_clip_pred, clip_distance, one })",
        "force_clip_pred, neg_one, selected_clip_distance",
    ):
        if required not in body:
            raise AssertionError(f"explicit clip-distance path is missing {required}")

    for required in (
        "const std::array<spv::Id, 6> clip_distances",
        "index < clip_distances.size()",
        "b.makeIntConstant(index)",
        "b.createStore(explicit_clip_distance, clip_distance_ref)",
    ):
        if required not in body:
            raise AssertionError(f"clip-distance store loop is missing {required}")

    for draw_source in (scene, gl_draw):
        if "vert_ublock.post_viewport_clip = 0.0f;" not in draw_source:
            raise AssertionError("draws must leave the post-viewport clip mode off")
        if "getenv" in draw_source and "VITA3K_ACXI" in draw_source:
            raise AssertionError("title-specific clip controls are back in a draw path")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
