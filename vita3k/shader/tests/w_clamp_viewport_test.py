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


def require(source: str, values: tuple[str, ...], label: str) -> None:
    for value in values:
        if value not in source:
            raise AssertionError(f"{label} is missing {value}")


def main() -> int:
    root = Path(sys.argv[1])
    gxm_types = (root / "vita3k/gxm/include/gxm/types.h").read_text()
    renderer_gxm_types = (root / "vita3k/renderer/include/renderer/gxm_types.h").read_text()
    renderer_types = (root / "vita3k/renderer/include/renderer/types.h").read_text()
    renderer_functions = (root / "vita3k/renderer/include/renderer/functions.h").read_text()
    renderer_source = (root / "vita3k/renderer/src/renderer.cpp").read_text()
    state_set = (root / "vita3k/renderer/src/state_set.cpp").read_text()
    gxm = (root / "vita3k/modules/SceGxm/SceGxm.cpp").read_text()
    uniform_block = (root / "vita3k/shader/include/shader/uniform_block.h").read_text()
    recompiler = (root / "vita3k/shader/src/spirv_recompiler.cpp").read_text()
    gl_draw = (root / "vita3k/renderer/src/gl/draw.cpp").read_text()
    vk_draw = (root / "vita3k/renderer/src/vulkan/scene.cpp").read_text()

    require(
        gxm_types,
        (
            "enum SceGxmWClampMode : uint32_t",
            "SCE_GXM_WCLAMP_MODE_DISABLED = 0x00000000u",
            "SCE_GXM_WCLAMP_MODE_ENABLED = 0x00008000u",
        ),
        "GXM W-clamp ABI",
    )
    require(
        renderer_gxm_types,
        (
            "SceGxmWClampMode w_clamp_mode = SCE_GXM_WCLAMP_MODE_ENABLED",
            "float w_clamp_value = 0.00001f",
        ),
        "GXM context defaults",
    )
    require(
        renderer_types,
        (
            "bool w_clamp_enabled = true",
            "float w_clamp_value = 0.00001f",
        ),
        "recorded renderer state",
    )

    update_viewport = function_body(
        gxm, "static void update_viewport(renderer::State &state, SceGxmContext *context)"
    )
    require(
        update_viewport,
        (
            "context->state.w_clamp_mode == SCE_GXM_WCLAMP_MODE_ENABLED",
            "context->state.w_clamp_value",
            "renderer::set_viewport_real",
            "renderer::set_viewport_flat",
        ),
        "viewport command propagation",
    )

    require(
        gxm,
        (
            "EXPORT(void, sceGxmSetWClampEnable, SceGxmContext *context, SceGxmWClampMode enable)",
            "EXPORT(void, sceGxmSetWClampValue, SceGxmContext *context, float clampValue)",
        ),
        "W-clamp exports",
    )
    enable = function_body(gxm, "EXPORT(void, sceGxmSetWClampEnable")
    value = function_body(gxm, "EXPORT(void, sceGxmSetWClampValue")
    require(
        enable,
        (
            "context->state.w_clamp_mode",
            "update_viewport(*emuenv.renderer, context)",
        ),
        "sceGxmSetWClampEnable",
    )
    require(
        value,
        (
            "context->state.w_clamp_value",
            "update_viewport(*emuenv.renderer, context)",
        ),
        "sceGxmSetWClampValue",
    )
    if "UNIMPLEMENTED" in enable or "UNIMPLEMENTED" in value:
        raise AssertionError("W-clamp exports remain unimplemented")

    require(
        renderer_functions + renderer_source,
        (
            "bool wClampEnabled",
            "float wClampValue",
            "GXMState::Viewport, false, wClampEnabled, wClampValue",
            "GXMState::Viewport, true, wClampEnabled, wClampValue",
        ),
        "renderer viewport command ABI",
    )
    viewport_handler = function_body(state_set, "COMMAND_SET_STATE(viewport) {")
    require(
        viewport_handler,
        (
            "const bool wClampEnabled = helper.pop<bool>()",
            "const float wClampValue = helper.pop<float>()",
            "render_context->record.w_clamp_enabled = wClampEnabled",
            "render_context->record.w_clamp_value = wClampValue",
        ),
        "render-thread W-clamp state",
    )

    require(
        uniform_block,
        (
            "float w_clamp_enabled",
            "float w_clamp_value",
            "float post_viewport_clip",
            "VERT_UNIFORM_w_clamp_enabled",
            "VERT_UNIFORM_w_clamp_value",
            "VERT_UNIFORM_post_viewport_clip",
        ),
        "vertex render uniform block",
    )
    require(
        recompiler,
        (
            "ADD_VERT_UNIFORM_MEMBER(w_clamp_enabled)",
            "ADD_VERT_UNIFORM_MEMBER(w_clamp_value)",
            "ADD_VERT_UNIFORM_MEMBER(post_viewport_clip)",
            "GLSLstd450FMax",
            "spv::OpSelect",
            "w_clamp_enabled",
            "w_clamp_value",
        ),
        "shared vertex finalizer",
    )
    scale_marker = recompiler.index("// Preserve the host clip coordinate while reproducing GXM\'s clamped viewport divisor.")
    branch_start = recompiler.rfind("cond_builder.makeBeginElse();", 0, scale_marker)
    branch_end = recompiler.index("cond_builder.makeEndIf();", scale_marker)
    if branch_start < 0:
        raise AssertionError("viewport-enabled vertex finalizer is missing")
    branch = recompiler[branch_start:branch_end]
    require(
        branch,
        (
            "w_clamp_enabled",
            "const spv::Id original_w = b.createCompositeExtract(o_val, f32, 3)",
            "const spv::Id viewport_divisor",
            "const spv::Id viewport_scale = b.createBinOp(spv::OpFDiv, f32, original_w, viewport_divisor)",
            "const spv::Id viewport_scale_v4 = b.createCompositeConstruct(v4, { viewport_scale, viewport_scale, one, one })",
            "const spv::Id host_clip_position = b.createBinOp(spv::OpFMul, out_type, o_val, viewport_scale_v4)",
            "const spv::Id post_viewport_clip_pred",
            "o_val = host_clip_position",
            "b.createBinOp(spv::OpFDiv, f32, z, viewport_divisor)",
            "const spv::Id unclamped_screen_z = z",
            "const spv::Id depth_clamped_z = b.createBuiltinCall(f32, utils.std_builtins, GLSLstd450FMax, { unclamped_screen_z, zero })",
            "z = b.createOp(spv::OpSelect, f32, { post_viewport_clip_pred, unclamped_screen_z, depth_clamped_z })",
            "b.createBinOp(spv::OpFMul, f32, z, original_w)",
        ),
        "viewport-enabled W clamp",
    )
    if "effective_clip_position" in branch:
        raise AssertionError("GL clip compatibility must not replace the host homogeneous clip coordinate")
    if "b.createStore(viewport_divisor, w_ref)" in branch or "b.createStore(effective_w, w_ref)" in branch:
        raise AssertionError("viewport W clamp must not replace the host homogeneous clip coordinate")
    if branch.index("const spv::Id viewport_divisor") > branch.index("b.createStore(o_val, out_var)"):
        raise AssertionError("viewport x/y are stored before applying the clamped divisor")
    if branch.index("const spv::Id viewport_divisor") > branch.index("b.createBinOp(spv::OpFDiv, f32, z, viewport_divisor)"):
        raise AssertionError("W is clamped after viewport depth division")

    shared_draw_requirements = (
        "vert_ublock.w_clamp_enabled = context.record.w_clamp_enabled ? 1.0f : 0.0f",
        "vert_ublock.w_clamp_value = context.record.w_clamp_value",
        "vert_ublock.post_viewport_clip = 0.0f",
    )
    for source, label in ((gl_draw, "OpenGL draw"), (vk_draw, "Vulkan draw")):
        require(source, shared_draw_requirements, label)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
