from pathlib import Path
import re
import sys


EXPECTED_HIGH_CROWD_HASH = bytes.fromhex(
    "b8cb8a1ddfcb88162ca87d4a87d27f0b8ea161ec50524c143ba9ee24d60f3cfe"
)
EXPECTED_LOW_CROWD_HASH = bytes.fromhex(
    "6ec1c320e585ac486eb37b911ded7d8842d0a012aa981abd2d3e8ad2e224b577"
)
EXPECTED_PLAYER_HASH = bytes.fromhex(
    "07d1d7b73bd20724542b57243dd99bdf7a86d0d8fbde7cb946eed764b50041e8"
)


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"inline bool {name}\(.*?^\}}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise AssertionError(f"missing {name}")
    return match.group(0)


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    target = function_body(source, "nba2k16_high_crowd_draw_probe_target")
    for required in (
        'std::getenv("VITA3K_NBA2K16_HIGH_CROWD_DRAW_PROBE")',
        'std::strcmp(value, "1") == 0',
        '"NBA2K1601"',
        "hash_size != sizeof(target_hash)",
        "std::memcmp(hash, target_hash, sizeof(target_hash)) == 0",
    ):
        if required not in target:
            raise AssertionError(f"NBA high-crowd probe target is missing {required}")

    hash_match = re.search(
        r"target_hash\[\]\s*=\s*\{(?P<body>.*?)\};",
        target,
        re.DOTALL,
    )
    if not hash_match:
        raise AssertionError("NBA high-crowd probe does not embed an exact target hash")
    values = bytes(
        int(token, 16)
        for token in re.findall(r"0x([0-9A-Fa-f]{2})", hash_match.group("body"))
    )
    if values != EXPECTED_HIGH_CROWD_HASH:
        raise AssertionError(f"wrong high-crowd shader hash: {values.hex()}")

    low_target = function_body(source, "nba2k16_low_crowd_draw_probe_target")
    for required in (
        'std::getenv("VITA3K_NBA2K16_LOW_CROWD_DRAW_PROBE")',
        'std::strcmp(value, "1") == 0',
        '"NBA2K1601"',
        "hash_size != sizeof(target_hash)",
        "std::memcmp(hash, target_hash, sizeof(target_hash)) == 0",
    ):
        if required not in low_target:
            raise AssertionError(f"NBA low-crowd probe target is missing {required}")

    low_hash_match = re.search(
        r"target_hash\[\]\s*=\s*\{(?P<body>.*?)\};",
        low_target,
        re.DOTALL,
    )
    if not low_hash_match:
        raise AssertionError("NBA low-crowd probe does not embed an exact target hash")
    low_values = bytes(
        int(token, 16)
        for token in re.findall(r"0x([0-9A-Fa-f]{2})", low_hash_match.group("body"))
    )
    if low_values != EXPECTED_LOW_CROWD_HASH:
        raise AssertionError(f"wrong low-crowd shader hash: {low_values.hex()}")

    player_target = function_body(source, "nba2k16_player_draw_probe_target")
    for required in (
        'std::getenv("VITA3K_NBA2K16_PLAYER_DRAW_PROBE")',
        'std::strcmp(value, "1") == 0',
        '"NBA2K1601"',
        "hash_size != sizeof(target_hash)",
        "std::memcmp(hash, target_hash, sizeof(target_hash)) == 0",
    ):
        if required not in player_target:
            raise AssertionError(f"NBA player probe target is missing {required}")

    player_hash_match = re.search(
        r"target_hash\[\]\s*=\s*\{(?P<body>.*?)\};",
        player_target,
        re.DOTALL,
    )
    if not player_hash_match:
        raise AssertionError("NBA player probe does not embed an exact target hash")
    player_values = bytes(
        int(token, 16)
        for token in re.findall(r"0x([0-9A-Fa-f]{2})", player_hash_match.group("body"))
    )
    if player_values != EXPECTED_PLAYER_HASH:
        raise AssertionError(f"wrong player shader hash: {player_values.hex()}")

    capture_start = source.index("inline bool capture_bios_draw")
    capture_end = source.index("\n}\n", capture_start)
    capture = source[capture_start:capture_end]
    target_call = capture.index("nba2k16_high_crowd_draw_probe_target")
    low_target_call = capture.index("nba2k16_low_crowd_draw_probe_target")
    player_target_call = capture.index("nba2k16_player_draw_probe_target")
    full_crowd = capture.index("nba2k16_full_crowd")
    gate = capture.index("!legacy_title && !nba2k16_full_crowd")
    visit = capture.index("probe.visit")
    if not target_call < low_target_call < player_target_call < full_crowd < gate < visit:
        raise AssertionError("exact NBA shader filtering must happen before probe activation")
    if "renderer_data->hash.data()" not in capture or "renderer_data->hash.size()" not in capture:
        raise AssertionError("probe target is not checked against the complete translated shader hash")
    for required in (
        "nba2k16_full_crowd ? index_bytes : std::min(index_bytes, size_t(512))",
        "nba2k16_full_crowd ? size : std::min<size_t>(size, 512)",
        "nba2k16_full_crowd ? 32768 : 8192",
    ):
        if required not in capture:
            raise AssertionError(f"high-crowd full-state capture is missing {required}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
