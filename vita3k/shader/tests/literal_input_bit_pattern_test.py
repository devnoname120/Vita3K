from pathlib import Path
import re
import sys


def main() -> int:
    types = Path(sys.argv[1]).read_text()
    parser = Path(sys.argv[2]).read_text()
    recompiler = Path(sys.argv[3]).read_text()

    literal_type = re.search(
        r"struct LiteralInputSource\s*\{(?P<body>.*?)\};",
        types,
        re.DOTALL,
    )
    if not literal_type or "std::uint32_t data_bits" not in literal_type.group("body"):
        raise AssertionError("GXP literal inputs must retain their raw 32-bit payload")

    if "std::bit_cast<std::uint32_t>(literals[i].data)" not in parser:
        raise AssertionError("the GXP parser converts literal payloads through host float semantics")

    visitor = re.search(
        r"\[&\]\(const LiteralInputSource &s\) \{(?P<body>.*?)\n\s*\},",
        recompiler,
        re.DOTALL,
    )
    if not visitor:
        raise AssertionError("literal-input SPIR-V translation is missing")
    body = visitor.group("body")
    for required in (
        "b.makeUintConstant(s.data_bits)",
        "b.createUnaryOp(spv::OpBitcast, f32_type, bits)",
    ):
        if required not in body:
            raise AssertionError(f"literal translation is missing {required}")
    if "makeFloatConstant" in body:
        raise AssertionError("literal payloads must not become foldable float constants")

    pack = re.search(
        r"auto create_new_literal_pack = \[&\]\(\) \{(?P<body>.*?)\n\s*\};",
        recompiler,
        re.DOTALL,
    )
    if not pack or "b.createCompositeConstruct" not in pack.group("body"):
        raise AssertionError("runtime literal bitcasts must be packed with OpCompositeConstruct")
    if "b.makeCompositeConstant" in pack.group("body"):
        raise AssertionError("runtime literal bitcasts cannot be packed as constants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
