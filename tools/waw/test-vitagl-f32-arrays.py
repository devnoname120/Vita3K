from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PREFIX = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdalign.h>
typedef enum {
    SCE_GXM_PARAMETER_TYPE_F32, SCE_GXM_PARAMETER_TYPE_F16,
    SCE_GXM_PARAMETER_TYPE_U32, SCE_GXM_PARAMETER_TYPE_S32,
    SCE_GXM_PARAMETER_TYPE_U16, SCE_GXM_PARAMETER_TYPE_S16,
    SCE_GXM_PARAMETER_TYPE_U8, SCE_GXM_PARAMETER_TYPE_S8
} SceGxmParameterType;
#define vgl_fast_memcpy memcpy
'''

TEST = r'''
static unsigned checked, failed;

static void compare(const unsigned char *actual, const unsigned char *expected,
                    size_t bytes, unsigned components, unsigned count, unsigned offset,
                    unsigned base) {
    ++checked;
    if (memcmp(actual, expected, bytes)) {
        ++failed;
        if (failed <= 8) {
            size_t i = 0;
            while (i < bytes && actual[i] == expected[i]) ++i;
            printf("MISMATCH components=%u count=%u offset=%u base=%u byte=%zu actual=%02x expected=%02x\n",
                   components, count, offset, base, i, actual[i], expected[i]);
        }
    }
}

int main(void) {
    alignas(16) unsigned char actual[1024], expected[1024];
    float values[64];
    uint32_t bits[64];
    for (unsigned i = 0; i < 64; ++i) bits[i] = 0x3f020301u + i * 0x10101u;
    memcpy(values, bits, sizeof(values));
    for (unsigned components = 1; components <= 4; ++components) {
        const unsigned stride = components == 3 ? 16 : components * 4;
        for (unsigned count = 0; count <= 12; ++count) {
            for (unsigned offset = 0; offset <= 6; ++offset) {
                for (unsigned base = 32; base <= 44; base += 4) {
                    for (unsigned i = 0; i < sizeof(actual); ++i)
                        actual[i] = expected[i] = (unsigned char)(i * 23 + 71);
                    for (unsigned i = 0; i < count; ++i)
                        memcpy(expected + base + (offset + i) * stride,
                               values + i * components, components * sizeof(float));
                    vglSetUniformData(actual + base, SCE_GXM_PARAMETER_TYPE_F32,
                                      (int)offset, count, components, values, SCE_GXM_PARAMETER_TYPE_F32);
                    compare(actual, expected, sizeof(actual), components, count, offset, base);
                }
            }
        }
    }
    memset(actual, 0xa5, sizeof(actual));
    const uint32_t warm_color[4] = {0x3f800000u, 0x3f28a8a9u, 0x3ea2a2a3u, 0};
    const uint32_t radius_words[3] = {0x39ffe004u, 0x8132e509u, 0x018e0c78u};
    float radii[3];
    memcpy(radii, radius_words, sizeof(radii));
    memcpy(actual + 32 * 4, warm_color, sizeof(warm_color));
    memcpy(expected, actual, sizeof(actual));
    memcpy(expected + 28 * 4, radii, sizeof(radii));
    vglSetUniformData(actual + 28 * 4, SCE_GXM_PARAMETER_TYPE_F32,
                      0, 3, 1, radii, SCE_GXM_PARAMETER_TYPE_F32);
    compare(actual, expected, sizeof(actual), 1, 3, 0, 28 * 4);
    uint32_t observed_red;
    memcpy(&observed_red, actual + 32 * 4, sizeof(observed_red));
    printf("CAPTURE_LAYOUT color_red=0x%08x expected=0x%08x radius_start_word=28 color_start_word=32\n",
           observed_red, warm_color[0]);
    printf("F32_LAYOUT_CASES checked=%u failed=%u\n", checked, failed);
    return failed ? 1 : 0;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.source.read_text()
    marker = "#define decode_unif_component_simple"
    if source.count(marker) != 1 or source.count("void vglSetUniformData(") != 1:
        raise RuntimeError("Unexpected VitaGL uniform writer layout")
    body = source[source.index(marker):]
    args.output.mkdir(parents=True, exist_ok=False)
    translation = args.output / "actual_writer_test.c"
    translation.write_text(PREFIX + body + TEST)
    executable = args.output / "actual-writer-test"
    command = ["clang", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", "-fno-sanitize-recover=all", str(translation), "-o", str(executable)]
    with (args.output / "compile.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=40)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
    (args.output / "test.log").write_text(result.stdout + result.stderr)
    report = {"source": str(args.source.resolve()),
              "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
              "command": command, "exit_code": result.returncode,
              "scope": "actual writer, F32-to-F32 components 1..4; counts, offsets, guard bytes and observed adjacent-uniform layout"}
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(result.stdout + result.stderr, end="")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
