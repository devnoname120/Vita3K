from pathlib import Path
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    required = (
        'VITA3K_SHADER_OVERRIDE_VERTEX_HASH',
        'VITA3K_SHADER_OVERRIDE_VERTEX_SPV',
        'VITA3K_SHADER_OVERRIDE_FRAGMENT_HASH',
        'VITA3K_SHADER_OVERRIDE_FRAGMENT_SPV',
        'diagnostic_hash == override_hash_value',
        'vkutil::load_shader(state.device, override_path)',
        'if (override_module)',
        'using translated shader',
        'precompile_shader(hash, false);',
    )
    for marker in required:
        if marker not in source:
            raise AssertionError(f"exact shader override path is missing {marker}")
    exact = source.index('const bool exact_override')
    load = source.index('vkutil::load_shader(state.device, override_path)', exact)
    fallback = source.index('precompile_shader(hash, false);', load)
    if not exact < load < fallback:
        raise AssertionError("override must be exact-hash gated and fail closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
