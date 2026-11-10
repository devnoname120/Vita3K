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
    source = Path(sys.argv[1]).read_text()
    texture_source = source[source.index("namespace texture {"):]
    swizzle4 = function_body(
        texture_source,
        "static vk::ComponentMapping translate_swizzle4(SceGxmTextureSwizzle4Mode mode)",
    )
    if not re.search(
        r"case\s+SCE_GXM_TEXTURE_SWIZZLE4_RGBA\s*:\s*return\s+swizzle_abgr\s*;",
        swizzle4,
    ):
        raise AssertionError("generic RGBA texture order must reverse the packed ABGR component layout")

    translate = function_body(
        texture_source,
        "vk::ComponentMapping translate_swizzle(SceGxmTextureFormat format)",
    )
    match = re.search(
        r"case\s+SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4\s*:(.*?)(?=\n\s*case|\n\s*default)",
        translate,
        re.DOTALL,
    )
    if not match:
        raise AssertionError("missing U4U4U4U4 texture swizzle case")
    if "return translate_swizzle4_abgr(" not in match.group(1):
        raise AssertionError("U4U4U4U4 must compensate for Vulkan R4G4B4A4 packed component order")
    if re.search(r"return\s+translate_swizzle4\s*\(", match.group(1)):
        raise AssertionError("U4U4U4U4 incorrectly uses the 8-bit four-component swizzle mapping")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
