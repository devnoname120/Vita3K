from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/waw-checks'


def function(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    for end in range(opening + 1, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise ValueError('Unterminated function: ' + signature)


vk_source = (ROOT / 'vita3k/renderer/src/vulkan/texture.cpp').read_text()
start = vk_source.index('\n', vk_source.index('    support_a2rgb10 =')) + 1
end = vk_source.index('    // check for dxt support', start)
capability = vk_source[start:end]
cache_source = (ROOT / 'vita3k/renderer/src/texture/cache.cpp').read_text()
start = cache_source.index('        case SCE_GXM_TEXTURE_BASE_FORMAT_PVRT2BPP:',
                           cache_source.index('void TextureCache::upload_texture('))
end = cache_source.index('        case SCE_GXM_TEXTURE_BASE_FORMAT_U8U3U3U2:', start)
conversion = cache_source[start:end]
mapper = function((ROOT / 'vita3k/renderer/src/vulkan/gxm_to_vulkan.cpp').read_text(),
                  'vk::Format translate_format(SceGxmTextureBaseFormat base_format)')
mapper = mapper.replace('"../texture/astc_formats.inc"',
                        '"' + str(ROOT / 'vita3k/renderer/src/texture/astc_formats.inc') + '"')
decoder = function((ROOT / 'vita3k/renderer/src/texture/format.cpp').read_text(),
                   'uint32_t decompress_compressed_texture(')
bpp = function((ROOT / 'vita3k/gxm/src/textures.cpp').read_text(),
               'uint32_t bits_per_pixel(SceGxmTextureBaseFormat base_format)')
program = r'''
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_format_traits.hpp>
#include <gxm/types.h>
#include <renderer/pvrt-dec.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#define LOG_ERROR(...) do {} while (0)
#define LOG_ERROR_ONCE(...) do {} while (0)
#define LOG_INFO_ONCE(...) do {} while (0)
namespace gxm {
''' + bpp + r'''
}
struct Device {
    bool advertised;
    vk::FormatProperties getFormatProperties(vk::Format) const {
        vk::FormatProperties properties{};
        if (advertised)
            properties.optimalTilingFeatures = vk::FormatFeatureFlagBits::eSampledImage;
        return properties;
    }
};
static bool native_upload(bool advertised) {
    struct { Device physical_device; } state{{advertised}};
    bool support_pvrt = true;
''' + capability + r'''
    return support_pvrt;
}
static void decompress_bc_image(uint32_t, uint32_t, const uint8_t *, uint32_t *, uint8_t) {
    assert(false && "The PVRTC fixture must not use a BC decoder");
}
''' + decoder + '\n' + mapper + r'''
static unsigned checks;
static void verify(bool advertised, SceGxmTextureBaseFormat base_format,
                   uint32_t width, uint32_t height, uint32_t color,
                   std::array<uint8_t, 4> expected) {
    const bool two_bit = gxm::bits_per_pixel(base_format) == 2;
    const uint32_t padded_width = std::max(width, two_bit ? 16u : 8u);
    const uint32_t padded_height = std::max(height, 8u);
    std::vector<uint32_t> compressed(padded_width * padded_height * (two_bit ? 2 : 4) / 32);
    for (size_t i = 0; i < compressed.size(); i += 2) {
        compressed[i] = 0;
        compressed[i + 1] = color;
    }
    const auto original = compressed;
    const bool support_pvrt = native_upload(advertised);
    const bool is_swizzled = true;
    const uint32_t pixels_per_stride = width;
    const uint32_t memory_height = height;
    const void *pixels = compressed.data();
    uint32_t bytes_per_pixel = 1;
    uint32_t bpp = gxm::bits_per_pixel(base_format);
    auto upload_format = base_format;
    std::vector<uint8_t> texture_data_decompressed;
    switch (base_format) {
''' + conversion + r'''
    default:
        assert(false);
    }
    if (upload_format != SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8) {
        std::fprintf(stderr, "PVRTC FORMAT MISMATCH: advertised=%u base=%08x output_bpp=%u, Vulkan image is RGBA8\n",
                     advertised, uint32_t(base_format), bpp);
        std::abort();
    }
    assert(translate_format(base_format) == vk::Format::eR8G8B8A8Unorm);
    assert(translate_format(upload_format) == translate_format(base_format));
    assert(gxm::bits_per_pixel(upload_format) == 32 && bpp == 32 && bytes_per_pixel == 4);
    assert(texture_data_decompressed.size() == size_t(width) * height * vk::blockSize(translate_format(base_format)));
    assert(pixels == texture_data_decompressed.data());
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        const auto *actual = static_cast<const uint8_t *>(pixels) + 4 * i;
        if (!std::equal(expected.begin(), expected.end(), actual)) {
            std::fprintf(stderr, "COLOR MISMATCH: base=%08x %ux%u block=%08x got=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                         uint32_t(base_format), width, height, color, actual[0], actual[1], actual[2], actual[3],
                         expected[0], expected[1], expected[2], expected[3]);
            std::abort();
        }
    }
    assert(compressed == original);
    ++checks;
}
int main() {
    const std::array formats{SCE_GXM_TEXTURE_BASE_FORMAT_PVRT2BPP, SCE_GXM_TEXTURE_BASE_FORMAT_PVRT4BPP,
                            SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII2BPP, SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII4BPP};
    const std::array<std::pair<uint32_t, std::array<uint8_t, 4>>, 5> colors{{
        {0xffe0ffe0, {255, 255, 0, 255}},
        {0xfc00fc00, {255, 0, 0, 255}},
        {0x83e083e0, {0, 255, 0, 255}},
        {0x801f801e, {0, 0, 255, 255}},
        {0, {0, 0, 0, 0}}
    }};
    const std::array<std::pair<uint32_t, uint32_t>, 7> sizes{{{1,1}, {2,2}, {4,4}, {8,8}, {16,8}, {16,16}, {32,16}}};
    for (bool advertised : {false, true})
        for (auto format : formats)
            for (const auto &[color, expected] : colors)
                for (const auto &[width, height] : sizes)
                    verify(advertised, format, width, height, color, expected);
    std::printf("%u PVRTC capability/conversion cases passed: four variants, GPU support on/off, RGB/alpha, dimensions and source preservation\n", checks);
}
'''
OUT.mkdir(parents=True, exist_ok=True)
source = OUT / 'pvrtc-upload-test.cpp'
source.write_text(program)
command = ['clang++', '-std=c++23', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
for directory in ('external/VulkanMemoryAllocator-Hpp/Vulkan-Headers/include', 'vita3k/gxm/include',
                  'vita3k/util/include', 'vita3k/renderer/include'):
    command += ['-I', str(ROOT / directory)]
command += [str(source), str(ROOT / 'vita3k/renderer/src/texture/pvrt-dec.cpp'), '-o', str(OUT / 'pvrtc-upload-test')]
subprocess.run(command, check=True)
subprocess.run([str(OUT / 'pvrtc-upload-test')], check=True)
