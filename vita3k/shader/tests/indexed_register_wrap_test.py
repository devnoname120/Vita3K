from pathlib import Path
import re
import sys


def block_body(source: str, signature: str) -> str:
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
    raise AssertionError(f"unterminated block: {signature}")


def effective_index(index_register: int, static_offset: int, double_registers: bool) -> int:
    scalar_scale = 2 if double_registers else 1
    return (index_register * scalar_scale + static_offset) & 0x7F


def main() -> int:
    root = Path(sys.argv[1])
    types = (root / "vita3k/shader/include/shader/usse_types.h").read_text()
    decoder = (root / "vita3k/shader/src/usse_decode_helpers.cpp").read_text()
    utilities = (root / "vita3k/shader/src/usse_utilities.cpp").read_text()
    version_header = (root / "vita3k/shader/include/shader/spirv_recompiler.h").read_text()

    operand = block_body(types, "struct Operand")
    if not re.search(r"bool\s+is_double_regs\s*\{\s*false\s*\}\s*;", operand):
        raise AssertionError("operands do not retain their hardware register-addressing unit")

    for signature, name in (
        ("Operand &decode_dest(", "dest"),
        ("Operand &decode_src12(", "src"),
        ("usse::Operand &decode_src0(", "src"),
    ):
        decoded = block_body(decoder, signature)
        if f"{name}.is_double_regs = is_double_regs;" not in decoded:
            raise AssertionError(f"{signature} discards the hardware register-addressing unit")

    load = block_body(utilities, "spv::Id load(")
    indexed = block_body(
        load,
        "if (op.bank == RegisterBank::INDEXED1 || op.bank == RegisterBank::INDEXED2)",
    )

    if "static constexpr int INDEXED_UNIFIED_STORE_MASK = 0x7F;" not in utilities:
        raise AssertionError("indexed Unified Store scalar addresses are not constrained to seven bits")

    if not re.search(
        r"const spv::Id scalar_idx\s*=\s*op\.is_double_regs\s*\?\s*"
        r"b\.createBinOp\(spv::OpIMul,\s*type_i32,\s*idx_reg_val,\s*b\.makeIntConstant\(2\)\)\s*"
        r":\s*idx_reg_val\s*;",
        indexed,
        re.DOTALL,
    ):
        raise AssertionError("indexed addressing does not distinguish 32-bit and 64-bit register units")

    raw = re.search(
        r"const spv::Id unwrapped_idx\s*=\s*b\.createBinOp\(spv::OpIAdd,\s*type_i32,\s*"
        r"scalar_idx,\s*b\.makeIntConstant\(add_off\)\s*\);",
        indexed,
        re.DOTALL,
    )
    if not raw:
        raise AssertionError("indexed addressing does not add the static offset after unit conversion")

    wrapped = re.search(
        r"const spv::Id real_idx\s*=\s*b\.createBinOp\(\s*spv::OpBitwiseAnd,\s*"
        r"type_i32,\s*unwrapped_idx,\s*b\.makeIntConstant\(INDEXED_UNIFIED_STORE_MASK\)\s*\);",
        indexed,
        re.DOTALL,
    )
    if not wrapped:
        raise AssertionError("indexed addressing does not wrap the base scalar address to the selected bank")

    friend = re.search(
        r"const spv::Id wrapped_friend_idx\s*=\s*b\.createBinOp\(\s*spv::OpBitwiseAnd,.*?"
        r"b\.makeIntConstant\(INDEXED_UNIFIED_STORE_MASK\).*?\);",
        indexed,
        re.DOTALL,
    )
    if not friend or "wrapped_friend_idx" not in indexed[indexed.index("idx_in_arr_2"):]:
        raise AssertionError("an indexed read crossing scalar register 127 does not wrap its second vec4 fetch")

    index_values = [20 * instance + 6 for instance in range(5)]
    scalar_actual = [effective_index(index, 15, False) for index in index_values]
    if scalar_actual != [21, 41, 61, 81, 101]:
        raise AssertionError(f"scalar indexed-address model changed: {scalar_actual}")

    vector_actual = [effective_index(index, 15, True) for index in index_values]
    if vector_actual != [27, 67, 107, 19, 59]:
        raise AssertionError(f"double-register indexed-address model changed: {vector_actual}")

    if ((127 + 3) & 0x7F) // 4 != 0:
        raise AssertionError("cross-bank companion fetch must wrap to vec4 zero")

    version_match = re.search(r"CURRENT_VERSION\s*=\s*(?P<version>\d+)\s*;", version_header)
    if not version_match or int(version_match.group("version")) < 16:
        raise AssertionError("indexed register-unit semantics must invalidate vk15 shader caches")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
