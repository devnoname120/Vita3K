#pragma once

#include "bounded_draw_probe.h"
#include <mem/functions.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace renderer::vulkan {

inline unsigned bios_draw_probe_limit() {
    const char *const value = std::getenv("VITA3K_BIOS_DRAW_PROBE_LIMIT");
    if (value && *value) {
        char *end = nullptr;
        const unsigned long requested_limit = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && requested_limit > 0 && requested_limit <= 4096)
            return static_cast<unsigned>(requested_limit);
    }
    return 512;
}

inline bool bios_draw_probe_next_frame() {
    const char *const value = std::getenv("VITA3K_BIOS_DRAW_PROBE_NEXT_FRAME");
    if (value && *value)
        return std::strcmp(value, "1") == 0;
    return false;
}

inline diagnostics::BoundedDrawProbe &bios_draw_probe() {
    static diagnostics::BoundedDrawProbe probe([] {
        const char *value = std::getenv("VITA3K_BIOS_DRAW_PROBE_DIR");
        return std::filesystem::path(value ? value : "");
    }(), bios_draw_probe_limit(), bios_draw_probe_next_frame());
    return probe;
}

inline void probe_hex(std::ostream &out, const uint8_t *bytes, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i)
        out << digits[bytes[i] >> 4] << digits[bytes[i] & 15];
    out << '\n';
}

inline bool probe_readable(const MemState &mem, Address address, size_t size) {
    const uint64_t end = uint64_t(address) + size;
    return size && address >= 4096 && end <= UINT32_MAX - 4096
        && is_valid_addr_range(mem, address, static_cast<Address>(end));
}

inline uint32_t probe_fnv1a(const uint8_t *bytes, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t index = 0; bytes && index < size; ++index)
        hash = (hash ^ bytes[index]) * 16777619u;
    return bytes ? hash : 0u;
}

inline uint32_t probe_alpha_nonzero(const uint8_t *bytes, size_t pixels) {
    uint32_t count = 0;
    for (size_t index = 0; bytes && index < pixels; ++index)
        count += bytes[index * 4u + 3u] != 0;
    return count;
}

inline bool nba2k16_high_crowd_draw_probe_target(const Config &config,
    const uint8_t *hash, size_t hash_size) {
    static constexpr uint8_t target_hash[] = {
        0xb8, 0xcb, 0x8a, 0x1d, 0xdf, 0xcb, 0x88, 0x16,
        0x2c, 0xa8, 0x7d, 0x4a, 0x87, 0xd2, 0x7f, 0x0b,
        0x8e, 0xa1, 0x61, 0xec, 0x50, 0x52, 0x4c, 0x14,
        0x3b, 0xa9, 0xee, 0x24, 0xd6, 0x0f, 0x3c, 0xfe,
    };
    const char *const value = std::getenv("VITA3K_NBA2K16_HIGH_CROWD_DRAW_PROBE");
    const bool enabled = value && std::strcmp(value, "1") == 0;
    if (!enabled || !config.run_app_path || *config.run_app_path != "NBA2K1601"
        || !hash || hash_size != sizeof(target_hash))
        return false;
    return std::memcmp(hash, target_hash, sizeof(target_hash)) == 0;
}

inline bool nba2k16_low_crowd_draw_probe_target(const Config &config,
    const uint8_t *hash, size_t hash_size) {
    static constexpr uint8_t target_hash[] = {
        0x6e, 0xc1, 0xc3, 0x20, 0xe5, 0x85, 0xac, 0x48,
        0x6e, 0xb3, 0x7b, 0x91, 0x1d, 0xed, 0x7d, 0x88,
        0x42, 0xd0, 0xa0, 0x12, 0xaa, 0x98, 0x1a, 0xbd,
        0x2d, 0x3e, 0x8a, 0xd2, 0xe2, 0x24, 0xb5, 0x77,
    };
    const char *const value = std::getenv("VITA3K_NBA2K16_LOW_CROWD_DRAW_PROBE");
    const bool enabled = value && std::strcmp(value, "1") == 0;
    if (!enabled || !config.run_app_path || *config.run_app_path != "NBA2K1601"
        || !hash || hash_size != sizeof(target_hash))
        return false;
    return std::memcmp(hash, target_hash, sizeof(target_hash)) == 0;
}

inline bool nba2k16_player_draw_probe_target(const Config &config,
    const uint8_t *hash, size_t hash_size) {
    static constexpr uint8_t target_hash[] = {
        0x07, 0xd1, 0xd7, 0xb7, 0x3b, 0xd2, 0x07, 0x24,
        0x54, 0x2b, 0x57, 0x24, 0x3d, 0xd9, 0x9b, 0xdf,
        0x7a, 0x86, 0xd0, 0xd8, 0xfb, 0xde, 0x7c, 0xb9,
        0x46, 0xee, 0xd7, 0x64, 0xb5, 0x00, 0x41, 0xe8,
    };
    const char *const value = std::getenv("VITA3K_NBA2K16_PLAYER_DRAW_PROBE");
    const bool enabled = value && std::strcmp(value, "1") == 0;
    if (!enabled || !config.run_app_path || *config.run_app_path != "NBA2K1601"
        || !hash || hash_size != sizeof(target_hash))
        return false;
    return std::memcmp(hash, target_hash, sizeof(target_hash)) == 0;
}

inline bool capture_bios_draw(VKContext &context, SceGxmPrimitiveType type,
    SceGxmIndexFormat format, Ptr<void> indices, size_t count, uint32_t instances,
    MemState &mem, const Config &config) {
    auto &probe = bios_draw_probe();
    const auto *const vertex_program = context.record.vertex_program.get(mem);
    const bool nba2k16_high_crowd = vertex_program
        && nba2k16_high_crowd_draw_probe_target(config,
            vertex_program->renderer_data->hash.data(),
            vertex_program->renderer_data->hash.size());
    const bool nba2k16_low_crowd = vertex_program
        && nba2k16_low_crowd_draw_probe_target(config,
            vertex_program->renderer_data->hash.data(),
            vertex_program->renderer_data->hash.size());
    const bool nba2k16_player = vertex_program
        && nba2k16_player_draw_probe_target(config,
            vertex_program->renderer_data->hash.data(),
            vertex_program->renderer_data->hash.size());
    const bool nba2k16_full_crowd = nba2k16_high_crowd || nba2k16_low_crowd || nba2k16_player;
    const bool legacy_title = config.run_app_path
        && (*config.run_app_path == "BIOS00001" || *config.run_app_path == "ACXI00001"
            || *config.run_app_path == "ASPHALT07" || *config.run_app_path == "DDRG00001"
            || *config.run_app_path == "MINIGORE2" || *config.run_app_path == "SIXT00001");
    if (!probe.enabled() || !config.run_app_path || (!legacy_title && !nba2k16_full_crowd))
        return false;
    static uint64_t doubledragon_target_frame = std::numeric_limits<uint64_t>::max();
    const bool doubledragon = *config.run_app_path == "DDRG00001";
    const bool acecombatxi = *config.run_app_path == "ACXI00001";
    if (doubledragon && doubledragon_target_frame == std::numeric_limits<uint64_t>::max()) {
        static constexpr uint8_t target_hash_prefix[] = { 0x2e, 0x41, 0xa6, 0x5f, 0xc8, 0x67, 0xfd, 0xc7 };
        const auto *const vertex = context.record.vertex_program.get(mem);
        if (!vertex || std::memcmp(vertex->renderer_data->hash.data(), target_hash_prefix, sizeof(target_hash_prefix)) != 0)
            return false;
        const auto &stream = context.record.vertex_streams[0];
        if (!stream.data || stream.size < 48 || !probe_readable(mem, stream.data.address(), 48))
            return false;
        const float *const values = stream.data.cast<const float>().get(mem);
        if (!values
            || values[0] != 0.0f || values[1] != 0.0f || values[2] != -80.0f
            || values[3] != 0.0f || values[4] != -320.0f || values[5] != -80.0f
            || values[6] != 480.0f || values[7] != -320.0f || values[8] != -80.0f
            || values[9] != 480.0f || values[10] != 0.0f || values[11] != -80.0f)
            return false;
        doubledragon_target_frame = context.frame_timestamp;
    }
    const auto result = probe.visit(context.frame_timestamp, [&](std::ostream &out, unsigned ordinal) {
        const auto &record = context.record;
        const auto &vertex = *record.vertex_program.get(mem);
        const auto &fragment_gxm = *record.fragment_program.get(mem);
        const auto &fragment = static_cast<const VKFragmentProgram &>(*fragment_gxm.renderer_data);
        const auto &fragment_program = *fragment_gxm.program.get(mem);
        const auto &blend = fragment.blending;
        out << fmt::format("DRAW {} frame={} scene={} count={} instances={} primitive={} index_format={} ubo_enabled={}\n",
            ordinal, context.frame_timestamp, context.scene_timestamp, count, instances,
            uint32_t(type), uint32_t(format), config.log_active_shaders);
        out << "SHADERS " << hex_string(vertex.renderer_data->hash) << ' ' << hex_string(fragment.hash) << '\n';
        out << fmt::format("TARGET {}x{} format={} writing_mask={} viewport_flat={} z_offset={} z_scale={} data={:08X} stride={} surface_type={} color_format={:08X}\n",
            context.render_target->width, context.render_target->height,
            uint32_t(context.current_color_format), record.writing_mask, record.viewport_flat,
            record.z_offset, record.z_scale, record.color_surface.data.address(),
            record.color_surface.strideInPixels, uint32_t(record.color_surface.surfaceType),
            uint32_t(record.color_surface.colorFormat));
        out << fmt::format("VIEWPORT x={} y={} width={} height={} min_depth={} max_depth={} scissor_x={} scissor_y={} scissor_width={} scissor_height={}\n",
            context.viewport.x, context.viewport.y, context.viewport.width, context.viewport.height,
            context.viewport.minDepth, context.viewport.maxDepth,
            context.scissor.offset.x, context.scissor.offset.y,
            context.scissor.extent.width, context.scissor.extent.height);
        out << fmt::format("RASTER cull={} two_sided={} clip_mode={} clip_min={},{} clip_max={},{} front_polygon={} back_polygon={} front_fragment={} back_fragment={}\n",
            uint32_t(record.cull_mode), uint32_t(record.two_sided), uint32_t(record.region_clip_mode),
            record.region_clip_min.x, record.region_clip_min.y,
            record.region_clip_max.x, record.region_clip_max.y,
            uint32_t(record.front_polygon_mode), uint32_t(record.back_polygon_mode),
            uint32_t(record.front_side_fragment_program_mode), uint32_t(record.back_side_fragment_program_mode));
        out << fmt::format("DEPTH surface_disabled={} format={} data={:08X} stencil={:08X} background={} front_func={} back_func={} front_write={} back_write={} bias_unit={} bias_slope={}\n",
            record.depth_stencil_surface.disabled(), uint32_t(record.depth_stencil_surface.get_format()),
            record.depth_stencil_surface.depth_data.address(), record.depth_stencil_surface.stencil_data.address(),
            record.depth_stencil_surface.background_depth,
            uint32_t(record.front_depth_func), uint32_t(record.back_depth_func),
            uint32_t(record.front_depth_write_mode), uint32_t(record.back_depth_write_mode),
            record.depth_bias_unit, record.depth_bias_slope);
        out << fmt::format("STENCIL front_func={} front_fail={} front_depth_fail={} front_pass={} front_compare={} front_write={} front_ref={} back_func={} back_fail={} back_depth_fail={} back_pass={} back_compare={} back_write={} back_ref={}\n",
            uint32_t(record.front_stencil_state_op.func), uint32_t(record.front_stencil_state_op.stencil_fail),
            uint32_t(record.front_stencil_state_op.depth_fail), uint32_t(record.front_stencil_state_op.depth_pass),
            record.front_stencil_state_values.compare_mask, record.front_stencil_state_values.write_mask,
            record.front_stencil_state_values.ref,
            uint32_t(record.back_stencil_state_op.func), uint32_t(record.back_stencil_state_op.stencil_fail),
            uint32_t(record.back_stencil_state_op.depth_fail), uint32_t(record.back_stencil_state_op.depth_pass),
            record.back_stencil_state_values.compare_mask, record.back_stencil_state_values.write_mask,
            record.back_stencil_state_values.ref);
        out << fmt::format("PROGRAM fragment_flags={:08X} fragment_no_effect={} frag_color_used={} maskupdate={} gamma={} vertex_textures={} fragment_textures={} pipeline={:016X} framebuffer={:016X} color_view={:016X} ds_view={:016X}\n",
            fragment_program.program_flags, fragment_program.has_no_effect(), fragment_program.is_frag_color_used(),
            fragment_gxm.is_maskupdate, record.is_gamma_corrected,
            vertex.renderer_data->texture_count, fragment.texture_count,
            uint64_t(reinterpret_cast<uintptr_t>(VkPipeline(context.current_pipeline))),
            uint64_t(reinterpret_cast<uintptr_t>(VkFramebuffer(context.current_framebuffer))),
            uint64_t(reinterpret_cast<uintptr_t>(VkImageView(context.current_color_view))),
            uint64_t(reinterpret_cast<uintptr_t>(VkImageView(context.current_ds_view))));
        out << fmt::format("BLEND enable={} src_rgb={} dst_rgb={} rgb_op={} src_a={} dst_a={} a_op={} mask={}\n",
            bool(blend.blendEnable), uint32_t(blend.srcColorBlendFactor), uint32_t(blend.dstColorBlendFactor),
            uint32_t(blend.colorBlendOp), uint32_t(blend.srcAlphaBlendFactor), uint32_t(blend.dstAlphaBlendFactor),
            uint32_t(blend.alphaBlendOp), uint32_t(VkColorComponentFlags(blend.colorWriteMask)));

        const auto textures = [&](const char *stage, const ShaderProgram &program, const vk::DescriptorImageInfo *bound) {
            for (unsigned unit = 0; unit < SCE_GXM_MAX_TEXTURE_UNITS; ++unit) {
                if (!program.textures_used.test(unit))
                    continue;
                out << "TEXTURE " << stage << ' ' << unit << " view=" << VkImageView(bound[unit].imageView);
                bool found = false;
                if (bound[unit].imageView) {
                    for (const auto &slot : context.state.texture_cache.texture_queue.items) {
                        const auto &info = slot.content;
                        if (!info.texture_size || info.index < 0 || size_t(info.index) >= context.state.texture_cache.textures.size())
                            continue;
                        const auto &image = context.state.texture_cache.textures[info.index].texture;
                        if (image.view != bound[unit].imageView)
                            continue;
                        const auto &texture = info.texture;
                        const Address address = texture.data_addr << 2;
                        const uint32_t width = gxm::get_width(texture);
                        const uint32_t height = gxm::get_height(texture);
                        const bool raw_readable = info.texture_size <= 8u * 1024u * 1024u
                            && probe_readable(mem, address, info.texture_size);
                        const uint8_t *const raw = raw_readable
                            ? Ptr<const uint8_t>(address).get(mem) : nullptr;
                        out << fmt::format(" hash={:016X} gxm_format={:08X} swizzle={} vk_format={} width={} height={} gamma={} address={:08X} raw_size={} raw_readable={} raw_fnv={:08X}",
                            info.hash, uint32_t(gxm::get_format(texture)), uint32_t(texture.swizzle_format),
                            uint32_t(image.format), width, height,
                            uint32_t(texture.gamma_mode), uint32_t(address), info.texture_size,
                            raw_readable, probe_fnv1a(raw, raw_readable ? info.texture_size : 0));
                        if (raw_readable && width && height
                            && gxm::get_base_format(gxm::get_format(texture)) == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8) {
                            const size_t row_bytes = size_t(align(width, 8u)) * 4u;
                            const uint32_t rows[] = { 0u, std::min(191u, height - 1u),
                                std::min(319u, height - 1u), std::min(320u, height - 1u), height - 1u };
                            for (uint32_t row : rows) {
                                const uint8_t *const row_data = raw + size_t(row) * row_bytes;
                                out << fmt::format(" row{}={:08X}/{}", row,
                                    probe_fnv1a(row_data, row_bytes),
                                    probe_alpha_nonzero(row_data, width));
                            }
                        }
                        found = true;
                        break;
                    }
                }
                out << " cache_match=" << found << '\n';
            }
        };
        textures("vertex", *vertex.renderer_data, context.vertex_textures);
        textures("fragment", fragment, context.fragment_textures);

        size_t uniform_budget = 8192;
        for (const auto &[binding, bytes] : context.ubo_data) {
            const size_t copied = std::min(uniform_budget, bytes.size());
            out << "UNIFORM " << binding << " bytes=" << bytes.size() << " copied=" << copied << '\n';
            probe_hex(out, bytes.data(), copied);
            uniform_budget -= copied;
        }

        uint32_t first_index = 0;
        uint32_t min_index = 0;
        uint32_t max_index = 0;
        const size_t index_size = format == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4;
        const size_t index_bytes = count <= SIZE_MAX / index_size ? count * index_size : 0;
        const size_t copied_indices = nba2k16_full_crowd ? index_bytes : std::min(index_bytes, size_t(512));
        const bool have_index = index_bytes && probe_readable(mem, indices.address(), index_bytes);
        if (have_index) {
            const uint8_t *index_data = indices.cast<const uint8_t>().get(mem);
            std::memcpy(&first_index, index_data, index_size);
            min_index = UINT32_MAX;
            if (format == SCE_GXM_INDEX_FORMAT_U16) {
                const uint16_t *values = reinterpret_cast<const uint16_t *>(index_data);
                for (size_t i = 0; i < count; ++i) {
                    min_index = std::min<uint32_t>(min_index, values[i]);
                    max_index = std::max<uint32_t>(max_index, values[i]);
                }
            } else {
                const uint32_t *values = reinterpret_cast<const uint32_t *>(index_data);
                for (size_t i = 0; i < count; ++i) {
                    min_index = std::min<uint32_t>(min_index, values[i]);
                    max_index = std::max(max_index, values[i]);
                }
            }
        }
        out << fmt::format("INDICES address={:08X} count={} bytes={} copied={} readable={} first={} min={} max={}\n",
            indices.address(), count, index_bytes, copied_indices, have_index, first_index, min_index, max_index);
        if (have_index)
            probe_hex(out, indices.cast<const uint8_t>().get(mem), copied_indices);
        out << fmt::format("RENDER_VERT flip={},{},{},{} flag={} screen={}x{}\n",
            record.viewport_flip[0], record.viewport_flip[1], record.viewport_flip[2], record.viewport_flip[3],
            record.viewport_flat ? 0.0f : 1.0f,
            context.render_target->width / context.state.res_multiplier,
            context.render_target->height / context.state.res_multiplier);

        const size_t vertex_info_size = context.curr_vert_ublock.get_size();
        const size_t fragment_info_size = context.curr_frag_ublock.get_size();
        const size_t vertex_uniform_size = vertex.renderer_data->max_total_uniform_buffer_storage * 4;
        const size_t fragment_uniform_size = fragment.max_total_uniform_buffer_storage * 4;
        out << fmt::format("RING_OFFSETS vertex_info={} fragment_info={} vertex_uniform={} fragment_uniform={} sizes={},{},{},{}\n",
            context.vertex_info_uniform_buffer.data_offset,
            context.fragment_info_uniform_buffer.data_offset,
            context.vertex_uniform_stream_ring_buffer.data_offset,
            context.fragment_uniform_stream_ring_buffer.data_offset,
            vertex_info_size, fragment_info_size, vertex_uniform_size, fragment_uniform_size);
        const auto dump_ring = [&](const char *name, const vkutil::HostRingBuffer &ring, size_t size) {
            const size_t copied = nba2k16_full_crowd ? size : std::min<size_t>(size, 512);
            const uint8_t *bytes = copied ? ring.mapped_at(ring.data_offset) : nullptr;
            const bool readable = bytes && uint64_t(ring.data_offset) + copied <= ring.mapped_size();
            out << fmt::format("RING {} copied={} readable={}\n", name, copied, readable);
            if (readable)
                probe_hex(out, bytes, copied);
        };
        dump_ring("vertex_info", context.vertex_info_uniform_buffer, vertex_info_size);
        dump_ring("fragment_info", context.fragment_info_uniform_buffer, fragment_info_size);
        dump_ring("vertex_uniform", context.vertex_uniform_stream_ring_buffer, vertex_uniform_size);
        dump_ring("fragment_uniform", context.fragment_uniform_stream_ring_buffer, fragment_uniform_size);
        for (const auto &attribute : vertex.attributes)
            out << fmt::format("ATTRIBUTE reg={} stream={} offset={} format={} components={}\n",
                attribute.regIndex, attribute.streamIndex, attribute.offset, uint32_t(attribute.format), attribute.componentCount);
        size_t vertex_budget = nba2k16_full_crowd ? 32768 : 8192;
        for (size_t index = 0; index < vertex.streams.size(); ++index) {
            const auto &stream = vertex.streams[index];
            const auto &info = record.vertex_streams[index];
            size_t element_size = 0;
            for (const auto &attribute : vertex.attributes) {
                if (attribute.streamIndex != index)
                    continue;
                const auto format = static_cast<SceGxmAttributeFormat>(attribute.format);
                element_size = std::max(element_size,
                    size_t(attribute.offset) + gxm::attribute_format_size(format) * attribute.componentCount);
            }
            if (!info.data || !vertex_budget || !element_size)
                continue;
            const bool constant = stream.stride == 0;
            const bool instanced = gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(stream.indexSource));
            const uint64_t offset = constant || instanced ? 0 : uint64_t(min_index) * stream.stride;
            const uint64_t address = uint64_t(info.data.address()) + offset;
            const size_t remaining = offset < info.size ? info.size - size_t(offset) : 0;
            const uint64_t requested_bytes_64 = constant ? element_size
                : instanced ? uint64_t(stream.stride) * std::min<uint32_t>(instances, 6u)
                            : ((uint64_t(max_index) - min_index) + 1u) * stream.stride;
            const size_t requested_bytes = requested_bytes_64 > SIZE_MAX
                ? SIZE_MAX : static_cast<size_t>(requested_bytes_64);
            const size_t copied = std::min({ remaining, requested_bytes, vertex_budget });
            const bool readable = have_index && address <= UINT32_MAX && probe_readable(mem, Address(address), copied);
            out << fmt::format("STREAM {} stride={} constant={} element_size={} index_source={} address={:X} size={} offset={} copied={} readable={}\n",
                index, stream.stride, constant, element_size, uint32_t(stream.indexSource),
                info.data.address(), info.size, offset, copied, readable);
            if (readable) {
                probe_hex(out, Ptr<const uint8_t>(Address(address)).get(mem), copied);
                vertex_budget -= copied;
            }
        }
    });
    if (result == diagnostics::BoundedDrawProbe::Result::completed) {
        if (doubledragon)
            doubledragon_target_frame = std::numeric_limits<uint64_t>::max();
        LOG_INFO("BIOS_DRAW_PROBE completed in VITA3K_BIOS_DRAW_PROBE_DIR");
    }
    else if (result == diagnostics::BoundedDrawProbe::Result::failed)
        LOG_ERROR("BIOS_DRAW_PROBE failed; any partial log is preserved");
    return true;
}

}
