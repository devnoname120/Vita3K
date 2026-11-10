// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <renderer/vulkan/functions.h>

#include <gxm/functions.h>
#include <renderer/vulkan/gxm_to_vulkan.h>

#include <config/state.h>
#include <spdlog/fmt/bin_to_hex.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#include "draw_probe_dump.h"
#include "nba2k16_crowd_uniform_trace.h"
#include "stream_ring_wrap_sync.h"
#include <util/log.h>

namespace renderer::vulkan {

static bool doubledragon_target_vertex_program(const ShaderProgram *program) {
    static constexpr uint8_t target_hash_prefix[] = { 0x2e, 0x41, 0xa6, 0x5f, 0xc8, 0x67, 0xfd, 0xc7 };
    return program && std::memcmp(program->hash.data(), target_hash_prefix, sizeof(target_hash_prefix)) == 0;
}

static bool doubledragon_viewport_trace_enabled() {
    const char *const value = std::getenv("VITA3K_DDRG_VIEWPORT_TRACE");
    return value && value[0] == '1' && value[1] == 0;
}

static bool acxi_skip_hangar_environment_draw(const VKContext &context, const MemState &mem, const Config &config) {
    if (config.run_app_path != "ACXI00001")
        return false;
    const char *const expected_hash = std::getenv("VITA3K_ACXI_SKIP_VERTEX_HASH");
    const char *const trigger = std::getenv("VITA3K_ACXI_SKIP_TRIGGER");
    const char *const range = std::getenv("VITA3K_ACXI_SKIP_DRAW_RANGE");
    const char *const keep_range = std::getenv("VITA3K_ACXI_KEEP_DRAW_RANGE");
    const bool keep_selected_range = keep_range && keep_range[0] == '1' && keep_range[1] == 0;
    if (!expected_hash || !*expected_hash || !trigger || !*trigger || !range || !*range)
        return false;
    std::error_code error;
    if (!std::filesystem::exists(trigger, error) || error)
        return false;

    std::string selected_range = range;
    std::ifstream control(trigger);
    std::string control_range;
    if (std::getline(control, control_range) && !control_range.empty())
        selected_range = std::move(control_range);
    const char *const range_begin = selected_range.data();
    const char *const range_end = range_begin + selected_range.size();
    const char *const separator = std::find(range_begin, range_end, ':');
    uint64_t first_draw = 0;
    uint64_t last_draw = 0;
    if (separator == range_begin || separator == range_end || separator + 1 == range_end)
        return false;
    const auto [first_end, first_error] = std::from_chars(range_begin, separator, first_draw);
    const auto [last_end, last_error] = std::from_chars(separator + 1, range_end, last_draw);
    if (first_error != std::errc() || last_error != std::errc()
        || first_end != separator || last_end != range_end
        || first_draw == 0 || last_draw < first_draw)
        return false;

    static thread_local uint64_t matching_scene_timestamp = 0;
    static thread_local uint64_t matching_draw_index = 0;
    if (matching_scene_timestamp != context.scene_timestamp) {
        matching_scene_timestamp = context.scene_timestamp;
        matching_draw_index = 0;
    }
    const auto *const vertex_program = context.record.vertex_program.get(mem);
    const bool matches_environment = vertex_program
        && hex_string(vertex_program->renderer_data->hash) == expected_hash;
    if (!matches_environment)
        return false;
    ++matching_draw_index;
    const bool inside_selected_range = matching_draw_index >= first_draw && matching_draw_index <= last_draw;
    const bool outside_selected_range = matching_draw_index < first_draw || matching_draw_index > last_draw;
    const bool skip = keep_selected_range ? outside_selected_range : inside_selected_range;
    if (skip && (matching_draw_index == first_draw || matching_draw_index == last_draw)) {
        std::fprintf(stderr,
            "VITA3K_ACXI_SKIP_HANGAR scene=%llu draw=%llu range=%llu:%llu vertex=%s\n",
            static_cast<unsigned long long>(context.scene_timestamp),
            static_cast<unsigned long long>(matching_draw_index),
            static_cast<unsigned long long>(first_draw),
            static_cast<unsigned long long>(last_draw), expected_hash);
        std::fflush(stderr);
    }
    return skip;
}

static bool nba2k16_skip_draw_range(const VKContext &context, const MemState &mem, const Config &config,
    SceGxmPrimitiveType type, size_t count, uint32_t instance_count) {
    if (config.run_app_path != "NBA2K1601")
        return false;

    static thread_local uint64_t scene_timestamp = std::numeric_limits<uint64_t>::max();
    static thread_local uint64_t draw_index = 0;
    if (scene_timestamp != context.scene_timestamp) {
        scene_timestamp = context.scene_timestamp;
        draw_index = 0;
    }
    ++draw_index;

    const char *const control_path = std::getenv("VITA3K_NBA2K16_DRAW_CONTROL");
    const char *const range = std::getenv("VITA3K_NBA2K16_DRAW_RANGE");
    const char *const keep_range = std::getenv("VITA3K_NBA2K16_KEEP_DRAW_RANGE");
    const bool keep_selected_range = keep_range && keep_range[0] == '1' && keep_range[1] == 0;
    if (!control_path || !*control_path || !range || !*range)
        return false;
    std::error_code error;
    if (!std::filesystem::exists(control_path, error) || error)
        return false;

    const auto *const vertex_program = context.record.vertex_program.get(mem);
    const auto *const fragment_program = context.record.fragment_program.get(mem);
    const std::string vertex_hash = vertex_program
        ? hex_string(vertex_program->renderer_data->hash) : std::string("none");
    const std::string fragment_hash = fragment_program
        ? hex_string(fragment_program->renderer_data->hash) : std::string("none");
    const char *const trace = std::getenv("VITA3K_NBA2K16_DRAW_TRACE");
    if (trace && trace[0] == '1' && trace[1] == 0) {
        std::fprintf(stderr,
            "VITA3K_NBA2K16_DRAW frame=%llu scene=%llu draw=%llu count=%zu instances=%u primitive=%u target=%ux%u vertex=%s fragment=%s\n",
            static_cast<unsigned long long>(context.frame_timestamp),
            static_cast<unsigned long long>(context.scene_timestamp),
            static_cast<unsigned long long>(draw_index), count, instance_count,
            static_cast<unsigned>(type), context.render_target->width,
            context.render_target->height, vertex_hash.c_str(), fragment_hash.c_str());
        std::fflush(stderr);
    }

    std::string selected_range = range;
    std::ifstream control(control_path);
    std::string control_range;
    if (std::getline(control, control_range) && !control_range.empty())
        selected_range = control_range;

    bool exact_shader_skip = false;
    bool exact_shader_selector = false;
    if (control_range.rfind("vertex:", 0) == 0) {
        exact_shader_selector = true;
        exact_shader_skip = vertex_hash == control_range.substr(7);
    } else if (control_range.rfind("fragment:", 0) == 0) {
        exact_shader_selector = true;
        exact_shader_skip = fragment_hash == control_range.substr(9);
    } else if (control_range.rfind("pair:", 0) == 0) {
        exact_shader_selector = true;
        const size_t pair_separator = control_range.find(':', 5);
        exact_shader_skip = pair_separator != std::string::npos
            && vertex_hash == control_range.substr(5, pair_separator - 5)
            && fragment_hash == control_range.substr(pair_separator + 1);
    }
    if (exact_shader_selector) {
        if (exact_shader_skip) {
            std::fprintf(stderr,
                "VITA3K_NBA2K16_DRAW_SHADER_SKIP scene=%llu draw=%llu selector=%s vertex=%s fragment=%s\n",
                static_cast<unsigned long long>(context.scene_timestamp),
                static_cast<unsigned long long>(draw_index), control_range.c_str(),
                vertex_hash.c_str(), fragment_hash.c_str());
            std::fflush(stderr);
        }
        return exact_shader_skip;
    }

    const char *const range_begin = selected_range.data();
    const char *const range_end = range_begin + selected_range.size();
    const char *const separator = std::find(range_begin, range_end, ':');
    uint64_t first_draw = 0;
    uint64_t last_draw = 0;
    if (separator == range_begin || separator == range_end || separator + 1 == range_end)
        return false;
    const auto [first_end, first_error] = std::from_chars(range_begin, separator, first_draw);
    const auto [last_end, last_error] = std::from_chars(separator + 1, range_end, last_draw);
    if (first_error != std::errc() || last_error != std::errc()
        || first_end != separator || last_end != range_end
        || first_draw == 0 || last_draw < first_draw)
        return false;

    const bool inside_selected_range = draw_index >= first_draw && draw_index <= last_draw;
    const bool outside_selected_range = draw_index < first_draw || draw_index > last_draw;
    const bool skip = keep_selected_range ? outside_selected_range : inside_selected_range;
    if (skip && (draw_index == first_draw || draw_index == last_draw)) {
        std::fprintf(stderr,
            "VITA3K_NBA2K16_DRAW_SKIP scene=%llu draw=%llu range=%llu:%llu keep=%u vertex=%s fragment=%s\n",
            static_cast<unsigned long long>(context.scene_timestamp),
            static_cast<unsigned long long>(draw_index),
            static_cast<unsigned long long>(first_draw),
            static_cast<unsigned long long>(last_draw), static_cast<unsigned>(keep_selected_range),
            vertex_hash.c_str(), fragment_hash.c_str());
        std::fflush(stderr);
    }
    return skip;
}

static int acxi_force_clip_plane(const Config &config, const Sha256Hash &vertex_hash) {
    if (config.run_app_path != "ACXI00001")
        return -1;
    const char *const control_path = std::getenv("VITA3K_ACXI_FORCE_CLIP_PLANE_FILE");
    const char *const expected_hash = std::getenv("VITA3K_ACXI_FORCE_CLIP_VERTEX_HASH");
    if (!control_path || !*control_path || !expected_hash || !*expected_hash
        || hex_string(vertex_hash) != expected_hash)
        return -1;

    std::ifstream control(control_path);
    std::string selection;
    if (!std::getline(control, selection) || selection.empty())
        return -1;
    int plane = -1;
    const char *const begin = selection.data();
    const char *const end = begin + selection.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, plane);
    if (error != std::errc() || parsed_end != end || plane < 0 || plane > 5)
        return -1;
    return plane;
}

static bool minigore_clear_trace_enabled() {
    const char *const value = std::getenv("VITA3K_MINIGORE_CLEAR_TRACE");
    return value && value[0] == '1' && value[1] == 0;
}

static bool minigore_clear_hash_pair(const ShaderProgram *vertex, const ShaderProgram *fragment) {
    static constexpr uint8_t vertex_hash_prefix[] = { 0x58, 0xe3, 0xc2, 0x8c, 0xf7, 0xfc, 0xd1, 0x90 };
    static constexpr uint8_t fragment_hash_prefix[] = { 0x48, 0xf3, 0xdf, 0x95, 0x16, 0x0b, 0x49, 0x79 };
    return vertex && fragment
        && std::memcmp(vertex->hash.data(), vertex_hash_prefix, sizeof(vertex_hash_prefix)) == 0
        && std::memcmp(fragment->hash.data(), fragment_hash_prefix, sizeof(fragment_hash_prefix)) == 0;
}

static void minigore_clear_trace_bytes(const uint8_t *bytes, size_t size) {
    for (size_t index = 0; bytes && index < size; ++index)
        std::fprintf(stderr, "%02X", bytes[index]);
    std::fputc('\n', stderr);
}

static void doubledragon_trace_bytes(const uint8_t *bytes, size_t size) {
    for (size_t index = 0; bytes && index < size; ++index)
        std::fprintf(stderr, "%02X", bytes[index]);
    std::fputc('\n', stderr);
}

void set_uniform_buffer(VKContext &context, MemState &mem, const ShaderProgram *program, const bool vertex_shader, const int block_num, const int size, Ptr<uint8_t> data) {
    auto offset = program->uniform_buffer_data_offsets.at(block_num);
    if (offset == static_cast<std::uint32_t>(-1)) {
        return;
    }

    const uint32_t data_size_upload = std::min<uint32_t>(size, program->uniform_buffer_sizes.at(block_num) * 4);
    static uint32_t doubledragon_uniform_trace_count = 0;
    const bool doubledragon_trace = vertex_shader && doubledragon_target_vertex_program(program)
        && doubledragon_viewport_trace_enabled() && doubledragon_uniform_trace_count < 16;
    const uint32_t doubledragon_call = doubledragon_trace ? ++doubledragon_uniform_trace_count : 0;
    const uint8_t *const source_bytes = data.get(mem);
    static uint32_t nba2k16_crowd_uniform_upload_count = 0;
    const bool nba2k16_crowd_trace = vertex_shader
        && !context.state.features.enable_memory_mapping
        && diagnostics::nba2k16_crowd_uniform_trace_enabled(program->hash.data(), program->hash.size())
        && nba2k16_crowd_uniform_upload_count < diagnostics::NBA2K16_CROWD_UNIFORM_TRACE_LIMIT;
    const uint32_t nba2k16_crowd_call = nba2k16_crowd_trace
        ? ++nba2k16_crowd_uniform_upload_count
        : 0;
    const auto source_hashes = nba2k16_crowd_trace
        ? diagnostics::nba2k16_crowd_uniform_hashes(source_bytes, data_size_upload)
        : diagnostics::Nba2k16CrowdUniformHashes{};
    if (doubledragon_trace) {
        std::fprintf(stderr,
            "VITA3K_DDRG_TARGET_UB_QUEUE call=%u block=%d guest=0x%08X requested=%d upload=%u float_offset=%u mapped=%u bytes=",
            doubledragon_call, block_num, data.address(), size, data_size_upload, offset,
            static_cast<unsigned>(context.state.features.enable_memory_mapping));
        doubledragon_trace_bytes(source_bytes, std::min<uint32_t>(data_size_upload, 256));
    }

    if (context.state.features.enable_memory_mapping) {
        if (context.state.mapping_method == MappingMethod::DoubleBuffer) {
            context.state.buffer_trapping.access_buffer(data.address(), data_size_upload, mem, false, true);
        }

        const uint64_t buffer_address = context.state.get_matching_device_address(data.address());
        if (vertex_shader) {
            context.curr_vert_ublock.set_buffer_address(block_num, buffer_address);
        } else {
            context.curr_frag_ublock.set_buffer_address(block_num, buffer_address);
        }
        if (doubledragon_trace) {
            std::fprintf(stderr,
                "VITA3K_DDRG_TARGET_UB_DEVICE call=%u block=%d address=0x%016llX\n",
                doubledragon_call, block_num, static_cast<unsigned long long>(buffer_address));
        }
    } else {
        const uint32_t offset_start_upload = offset * 4;

        if (vertex_shader) {
            if (!context.vertex_uniform_storage_allocated) {
                context.vertex_uniform_stream_ring_buffer.allocate(program->max_total_uniform_buffer_storage * 4);
                context.vertex_uniform_storage_allocated = true;
            }

            context.vertex_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload, source_bytes, offset_start_upload);
            if (nba2k16_crowd_trace) {
                const uint32_t ring_base = context.vertex_uniform_stream_ring_buffer.data_offset;
                const uint32_t ring_offset = ring_base + offset_start_upload;
                const uint32_t expected_bytes = program->uniform_buffer_sizes.at(block_num) * 4u;
                const uint32_t total_bytes = program->max_total_uniform_buffer_storage * 4u;
                const uint8_t *const ring_bytes = context.vertex_uniform_stream_ring_buffer.mapped_at(ring_offset);
                const bool ring_readable = ring_bytes
                    && uint64_t(ring_offset) + diagnostics::NBA2K16_CROWD_UNIFORM_BYTES
                        <= context.vertex_uniform_stream_ring_buffer.mapped_size();
                const auto ring_hashes = ring_readable
                    ? diagnostics::nba2k16_crowd_uniform_hashes(ring_bytes, diagnostics::NBA2K16_CROWD_UNIFORM_BYTES)
                    : diagnostics::Nba2k16CrowdUniformHashes{};
                const bool equal = ring_readable
                    && data_size_upload >= diagnostics::NBA2K16_CROWD_UNIFORM_BYTES
                    && std::memcmp(source_bytes, ring_bytes, diagnostics::NBA2K16_CROWD_UNIFORM_BYTES) == 0;
                std::fprintf(stderr,
                    "VITA3K_NBA2K16_CROWD_UB_UPLOAD call=%u block=%d guest=0x%08X requested=%d expected=%u upload=%u float_offset=%u total=%u base=%u offset=%u readable=%u equal=%u source=%016llX source_instance=%016llX source_matrix=%016llX ring=%016llX ring_instance=%016llX ring_matrix=%016llX\n",
                    nba2k16_crowd_call, block_num, data.address(), size, expected_bytes,
                    data_size_upload, offset, total_bytes, ring_base, ring_offset,
                    static_cast<unsigned>(ring_readable), static_cast<unsigned>(equal),
                    static_cast<unsigned long long>(source_hashes.full),
                    static_cast<unsigned long long>(source_hashes.instance),
                    static_cast<unsigned long long>(source_hashes.matrix),
                    static_cast<unsigned long long>(ring_hashes.full),
                    static_cast<unsigned long long>(ring_hashes.instance),
                    static_cast<unsigned long long>(ring_hashes.matrix));
                std::fflush(stderr);
            }
            if (doubledragon_trace) {
                const uint32_t ring_offset = context.vertex_uniform_stream_ring_buffer.data_offset + offset_start_upload;
                const uint8_t *const ring_bytes = context.vertex_uniform_stream_ring_buffer.mapped_at(ring_offset);
                const bool readable = ring_bytes
                    && uint64_t(ring_offset) + data_size_upload <= context.vertex_uniform_stream_ring_buffer.mapped_size();
                std::fprintf(stderr,
                    "VITA3K_DDRG_TARGET_UB_RING call=%u block=%d base=%u offset=%u readable=%u bytes=",
                    doubledragon_call, block_num, context.vertex_uniform_stream_ring_buffer.data_offset,
                    ring_offset, static_cast<unsigned>(readable));
                doubledragon_trace_bytes(readable ? ring_bytes : nullptr,
                    readable ? std::min<uint32_t>(data_size_upload, 256) : 0);
            }
        } else {
            if (!context.fragment_uniform_storage_allocated) {
                context.fragment_uniform_stream_ring_buffer.allocate(program->max_total_uniform_buffer_storage * 4);
                context.fragment_uniform_storage_allocated = true;
            }

            context.fragment_uniform_stream_ring_buffer.copy(context.prerender_cmd, data_size_upload, source_bytes, offset_start_upload);
        }
    }
    if (doubledragon_trace)
        std::fflush(stderr);
}

void mid_scene_flush(VKContext &context, const SceGxmNotification notification) {
    // two cases :
    // notification.addr is 0: this means that the mid scene flush must be used as a barrier in the renderpass
    // notification.addr is not 0: this means the app is waiting for this part to be finished to re-use the resources

    // Note: however, when testing, the barrier inside a pipeline does not work (or not entirely, depending on the GPU)
    // maybe because I'm writing using buffer device addresses, not sure...
    // so for the time being always restart the render pass
    // const bool restart_render_pass = notification.address.address() != 0;
    const bool restart_render_pass = true;

    if (restart_render_pass && context.in_renderpass)
        context.stop_render_pass();

    // in case there is no notification, this will happen in the render pass
    vk::MemoryBarrier barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead,
    };
    context.render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eVertexShader, vk::PipelineStageFlagBits::eVertexInput,
        vk::DependencyFlags(), barrier, {}, {});

    if (restart_render_pass) {
        SceGxmNotification empty_notification = { Ptr<uint32_t>(0), 0 };
        const bool submit = notification.address.address() != 0;
        context.stop_recording(notification, empty_notification, submit);
        context.start_recording();
        context.scene_timestamp++;
    }
}

#ifdef __APPLE__
// restride vertex attribute binding strides to multiple of 4
// needed for metal because it only allows multiples of 4.
void restride_stream(const uint8_t *&stream, uint32_t &size, uint32_t stride) {
    const uint32_t new_stride = align(stride, 4);
    const uint32_t nb_vertex_input = ((size + stride - 1) / stride);

    uint8_t *new_data = new uint8_t[nb_vertex_input * new_stride];
    for (uint32_t i = 0; i < nb_vertex_input; i++) {
        memcpy(new_data + new_stride * i, stream + stride * i, stride);
    }

    stream = new_data;
    size = nb_vertex_input * new_stride;
}
#endif

// when needed, how many descriptor of the given size we allocate for each frame at once
static constexpr uint32_t DESCRIPTOR_PACK_SIZE = 64;

static vk::DescriptorSet retrieve_descriptor(VKContext &context, bool is_vertex, uint16_t textures_count) {
    if (textures_count == 0)
        return context.empty_set;

    VKState &state = context.state;
    FrameDescriptor &frame_descriptor = is_vertex ? state.frame().vert_descriptors[textures_count - 1] : state.frame().frag_descriptors[textures_count - 1];
    if (frame_descriptor.descriptors_idx < frame_descriptor.sets.size())
        return frame_descriptor.sets[frame_descriptor.descriptors_idx++];

    // we have no more frame descriptor available, create a bunch of new one for this specific layout
    vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = textures_count * DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING
    };

    vk::DescriptorPoolCreateInfo descriptor_pool_info{
        .maxSets = DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING
    };
    descriptor_pool_info.setPoolSizes(pool_size);

    vk::DescriptorPool descriptor_pool = state.device.createDescriptorPool(descriptor_pool_info);
    state.frame_descriptor_pools.push_back(descriptor_pool);

    // allocate all the descriptor sets
    const vk::DescriptorSetLayout set_layout = is_vertex ? state.pipeline_cache.vertex_textures_layout[textures_count] : state.pipeline_cache.fragment_textures_layout[textures_count];
    std::vector<vk::DescriptorSetLayout> layouts(DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING, set_layout);
    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool
    };
    descr_set_info.setSetLayouts(layouts);
    auto descriptor_sets = state.device.allocateDescriptorSets(descr_set_info);

    // distribute them among all frames
    for (int frame_idx = 0; frame_idx < MAX_FRAMES_RENDERING; frame_idx++) {
        FrameObject &frame_object = state.frames[frame_idx];
        FrameDescriptor &frame_descr = is_vertex ? frame_object.vert_descriptors[textures_count - 1] : frame_object.frag_descriptors[textures_count - 1];

        // insert DESCRIPTOR_PACK_SIZE in each frame descriptor
        auto descr_it = descriptor_sets.begin() + frame_idx * DESCRIPTOR_PACK_SIZE;
        frame_descr.sets.insert(frame_descr.sets.end(), descr_it, descr_it + DESCRIPTOR_PACK_SIZE);
    }

    return frame_descriptor.sets[frame_descriptor.descriptors_idx++];
}

static void draw_bind_descriptors(VKContext &context, MemState &mem) {
    VKState &state = context.state;

    std::array<vk::DescriptorSet, 4> descriptors;
    descriptors[0] = context.global_set;
    descriptors[1] = context.rendertarget_set;

    const uint16_t vertex_textures_count = context.record.vertex_program.get(mem)->renderer_data->texture_count;
    const uint16_t fragment_texture_count = context.record.fragment_program.get(mem)->renderer_data->texture_count;

    vk::PipelineLayout pipeline_layout = state.pipeline_cache.pipeline_layouts[vertex_textures_count][fragment_texture_count];

    // try to use last descriptor if it still matches
    bool need_vert_descr = (vertex_textures_count != context.last_vert_texture_count);
    bool need_frag_descr = (fragment_texture_count != context.last_frag_texture_count);

    context.last_vert_texture_count = vertex_textures_count;
    context.last_frag_texture_count = fragment_texture_count;

    {
        if (need_vert_descr) {
            context.last_vert_texture_descriptor = retrieve_descriptor(context, true, vertex_textures_count);
        }
        descriptors[2] = context.last_vert_texture_descriptor;

        if (need_frag_descr) {
            context.last_frag_texture_descriptor = retrieve_descriptor(context, false, fragment_texture_count);
        }
        descriptors[3] = context.last_frag_texture_descriptor;
    }

    // bind textures
    std::array<vk::WriteDescriptorSet, 16> write_descrs;
    // some default sampler in case a slot has never been set and we read a slot with higher idx
    vk::DescriptorImageInfo default_image_info{
        .sampler = context.state.default_image.sampler,
        .imageView = context.state.default_image.view,
        .imageLayout = vk::ImageLayout::eGeneral
    };

    // vertex
    if (need_vert_descr) {
        for (uint32_t i = 0; i < vertex_textures_count; i++) {
            write_descrs[i] = vk::WriteDescriptorSet{
                .dstSet = descriptors[2],
                .dstBinding = i,
                .dstArrayElement = 0,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            };
            write_descrs[i].setImageInfo(context.vertex_textures[i].sampler ? context.vertex_textures[i] : default_image_info);
        }
        state.device.updateDescriptorSets(vertex_textures_count, write_descrs.data(), 0, nullptr);
    }

    // fragment
    if (need_frag_descr) {
        for (uint32_t i = 0; i < fragment_texture_count; i++) {
            write_descrs[i] = vk::WriteDescriptorSet{
                .dstSet = descriptors[3],
                .dstBinding = i,
                .dstArrayElement = 0,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            };
            write_descrs[i].setImageInfo(context.fragment_textures[i].sampler ? context.fragment_textures[i] : default_image_info);
        }
        state.device.updateDescriptorSets(fragment_texture_count, write_descrs.data(), 0, nullptr);
    }

    const uint32_t dynamic_offset_count = state.features.enable_memory_mapping ? 2U : 4U;
    const uint32_t dynamic_offsets[] = {
        // GXMRenderVertUniformBlock
        context.vertex_info_uniform_buffer.data_offset,
        // GXMRenderFragUniformBlock
        context.fragment_info_uniform_buffer.data_offset,
        // vertex ssbo
        context.vertex_uniform_stream_ring_buffer.data_offset,
        // fragment ssbo
        context.fragment_uniform_stream_ring_buffer.data_offset
    };

    context.render_cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
        descriptors.size(), descriptors.data(), dynamic_offset_count, dynamic_offsets);
}

static int active_vertex_stream_count(const VKContext &context, const MemState &mem) {
    const SceGxmVertexProgram &vertex_program = *context.record.vertex_program.get(mem);
    const VertexProgram *vkvert = vertex_program.renderer_data.get();
    int max_stream_idx = -1;
    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        if (vkvert->attribute_infos.contains(attribute.regIndex))
            max_stream_idx = std::max<int>(max_stream_idx, attribute.streamIndex);
    }
    return max_stream_idx + 1;
}

static uint32_t vertex_stream_upload_bytes(const VKContext &context, const MemState &mem) {
    const SceGxmVertexProgram &vertex_program = *context.record.vertex_program.get(mem);
    const int stream_count = active_vertex_stream_count(context, mem);
    uint64_t total = 0;
    for (int i = 0; i < stream_count; ++i) {
        if (!context.record.vertex_streams[i].data)
            continue;
        uint32_t stream_size = context.record.vertex_streams[i].size;
#ifdef __APPLE__
        const uint32_t stride = vertex_program.streams[i].stride;
        if (stride % 4 != 0) {
            const uint32_t new_stride = align(stride, 4);
            const uint32_t vertex_count = (stream_size + stride - 1) / stride;
            stream_size = vertex_count * new_stride;
        }
#endif
        total += align(stream_size, context.vertex_stream_ring_buffer.alignment);
    }
    return static_cast<uint32_t>(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

static void synchronize_stream_ring_wraps(VKContext &context, uint32_t vertex_bytes, uint32_t index_bytes) {
    if (!diagnostics::stream_ring_wrap_sync_enabled())
        return;

    const bool vertex_wrap = vertex_bytes != 0 && context.vertex_stream_ring_buffer.will_wrap(vertex_bytes);
    const bool index_wrap = index_bytes != 0 && context.index_stream_ring_buffer.will_wrap(index_bytes);
    if (!vertex_wrap && !index_wrap)
        return;

    const bool was_recording = context.is_recording;
    if (was_recording) {
        SceGxmNotification empty_notification{};
        context.stop_recording(empty_notification, empty_notification, true);
    }
    context.state.device.waitIdle();

    static uint32_t trace_count = 0;
    if (trace_count < 128u) {
        if (vertex_wrap) {
            std::fprintf(stderr,
                "VITA3K_STREAM_RING_WRAP_SYNC kind=vertex frame=%llu cursor=%u size=%u capacity=%u\n",
                static_cast<unsigned long long>(context.frame_timestamp),
                context.vertex_stream_ring_buffer.used_bytes(), vertex_bytes,
                context.vertex_stream_ring_buffer.capacity_bytes());
            ++trace_count;
        }
        if (index_wrap && trace_count < 128u) {
            std::fprintf(stderr,
                "VITA3K_STREAM_RING_WRAP_SYNC kind=index frame=%llu cursor=%u size=%u capacity=%u\n",
                static_cast<unsigned long long>(context.frame_timestamp),
                context.index_stream_ring_buffer.used_bytes(), index_bytes,
                context.index_stream_ring_buffer.capacity_bytes());
            ++trace_count;
        }
        std::fflush(stderr);
    }

    if (was_recording)
        context.start_recording();
}

// vertex count is only used with double buffer mapping
static void bind_vertex_streams(VKContext &context, MemState &mem, uint32_t instance_count, uint32_t max_index) {
    GxmRecordState &state = context.record;
    const SceGxmVertexProgram &vertex_program = *state.vertex_program.get(mem);
    const int max_stream_idx = active_vertex_stream_count(context, mem);

    if (context.state.mapping_method == MappingMethod::DoubleBuffer) {
        for (int i = 0; i < max_stream_idx; i++)
            state.vertex_streams[i].size = 0;

        // same as in SceGxm.cpp
        for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
            const SceGxmAttributeFormat attribute_format = static_cast<SceGxmAttributeFormat>(attribute.format);
            const size_t attribute_size = gxm::attribute_format_size(attribute_format) * attribute.componentCount;
            const SceGxmVertexStream &stream = vertex_program.streams[attribute.streamIndex];
            const SceGxmIndexSource index_source = static_cast<SceGxmIndexSource>(stream.indexSource);
            const size_t data_passed_length = gxm::is_stream_instancing(index_source) ? ((instance_count - 1) * stream.stride) : (max_index * stream.stride);
            const size_t data_length = attribute.offset + data_passed_length + attribute_size;
            state.vertex_streams[attribute.streamIndex].size = std::max<size_t>(state.vertex_streams[attribute.streamIndex].size, data_length);
        }

        for (int i = 0; i < max_stream_idx; i++) {
            if (state.vertex_streams[i].data)
                // on the PS Vita, shader stores are used most of the time to write to a vertex buffer
                context.state.buffer_trapping.access_buffer(state.vertex_streams[i].data.address(), static_cast<uint32_t>(state.vertex_streams[i].size), mem, context.state.has_shader_store);
        }
    }

    if (max_stream_idx == 0)
        return;

    for (int i = 0; i < max_stream_idx; i++) {
        if (state.vertex_streams[i].data) {
            if (context.state.features.enable_memory_mapping) {
                auto [buffer, offset] = context.state.get_matching_mapping(state.vertex_streams[i].data.cast<void>());

                context.vertex_stream_offsets[i] = offset;
                context.vertex_stream_buffers[i] = buffer;
            } else {
                const uint8_t *stream = state.vertex_streams[i].data.get(mem);
                uint32_t stream_size = state.vertex_streams[i].size;
#ifdef __APPLE__
                // Vulkan allows any stride, but Metal only allows multiples of 4.
                const bool restride = vertex_program.streams[i].stride % 4 != 0;
                if (restride) {
                    restride_stream(stream, stream_size, vertex_program.streams[i].stride);
                }
#endif
                context.vertex_stream_ring_buffer.allocate(context.prerender_cmd, stream_size, stream);
                context.vertex_stream_offsets[i] = context.vertex_stream_ring_buffer.data_offset;

#ifdef __APPLE__
                if (restride) {
                    delete[] stream;
                }
#endif
            }

            state.vertex_streams[i].data = nullptr;
            state.vertex_streams[i].size = 0;
        }
    }

    context.render_cmd.bindVertexBuffers(0, max_stream_idx, context.vertex_stream_buffers, context.vertex_stream_offsets);
}

void draw(VKContext &context, SceGxmPrimitiveType type, SceGxmIndexFormat format,
    Ptr<void> indices, size_t count, uint32_t instance_count, MemState &mem, const Config &config) {
    // The uniform ring slots of a draw are sized for its own programs. Release
    // them on every exit, including the early returns for a dropped mask-update
    // draw or a pipeline still compiling: otherwise the next draw writes its
    // uniforms into the smaller slot and the draw after it overwrites them.
    // VitaGL issues a mask-update draw for every scissor change and at the
    // start of each scene, so the first draw after one rendered with corrupted
    // uniforms (the missing first glyph or panel of a scissored region).
    struct UniformStorageRelease {
        VKContext &context;
        ~UniformStorageRelease() {
            context.vertex_uniform_storage_allocated = false;
            context.fragment_uniform_storage_allocated = false;
        }
    } uniform_storage_release{ context };
    const auto &mg2_vertex_renderer = context.record.vertex_program.get(mem)->renderer_data;
    const auto &mg2_fragment_renderer = context.record.fragment_program.get(mem)->renderer_data;
    static uint32_t mg2_clear_trace_count = 0;
    const bool mg2_clear_trace = minigore_clear_trace_enabled()
        && minigore_clear_hash_pair(mg2_vertex_renderer.get(), mg2_fragment_renderer.get())
        && mg2_clear_trace_count < 64;
    const uint32_t mg2_clear_call = mg2_clear_trace ? ++mg2_clear_trace_count : 0;
    const SceGxmFragmentProgram &gxm_fragment_program = *context.record.fragment_program.get(mem);
    if (mg2_clear_trace) {
        std::fprintf(stderr,
            "VITA3K_MG2_CLEAR_ENTRY call=%u count=%zu instances=%u primitive=%u format=%u indices=0x%08X maskupdate=%u use_mask=%u refresh=%u pipeline=%p renderpass=%u viewport=%g,%g,%g,%g scissor=%d,%d,%u,%u\n",
            mg2_clear_call, count, instance_count, uint32_t(type), uint32_t(format), indices.address(),
            unsigned(gxm_fragment_program.is_maskupdate), unsigned(context.state.features.use_mask_bit),
            unsigned(context.refresh_pipeline), VkPipeline(context.current_pipeline), unsigned(context.in_renderpass),
            context.viewport.x, context.viewport.y, context.viewport.width, context.viewport.height,
            context.scissor.offset.x, context.scissor.offset.y,
            context.scissor.extent.width, context.scissor.extent.height);
        std::fflush(stderr);
    }
    if (!context.state.features.use_mask_bit && gxm_fragment_program.is_maskupdate) {
        if (mg2_clear_trace) {
            std::fprintf(stderr, "VITA3K_MG2_CLEAR_GATE call=%u gate=maskupdate\n", mg2_clear_call);
            std::fflush(stderr);
        }
        return;
    }

    void *indices_ptr = indices.get(mem);
    const bool use_memory_mapping = context.state.features.enable_memory_mapping;
    const size_t index_size = (format == SCE_GXM_INDEX_FORMAT_U16) ? 2 : 4;
    const size_t index_buffer_size = index_size * count;
    if (!use_memory_mapping) {
        synchronize_stream_ring_wraps(context,
            vertex_stream_upload_bytes(context, mem),
            static_cast<uint32_t>(index_buffer_size));
    }

    context.check_for_macroblock_change(true);

    if (!context.in_renderpass)
        context.start_render_pass();

    // when we do multiple render pass for one scene (shader interlock or slow macroblock),
    // we need to always load the depth-stencil after the first draw
    if (context.is_first_scene_draw && (context.state.features.support_shader_interlock || context.ignore_macroblock)) {
        // update the render pass to load and store the depth and stencil
        context.current_render_pass = context.state.pipeline_cache.retrieve_render_pass(context.current_color_format, true, true, !context.record.color_surface.data);
        context.is_first_scene_draw = false;
    }

    const SceGxmProgram &fragment_program_gxp = *gxm_fragment_program.program.get(mem);
    if (context.state.features.direct_fragcolor && fragment_program_gxp.is_frag_color_used()) {
        // the fragment shader is using programmable blending with a subpass input
        vk::ImageMemoryBarrier barrier{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = context.current_color_base_image->image,
            .subresourceRange = vkutil::color_subresource_range
        };
        context.render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlagBits::eByRegion, {}, {}, barrier);
    } else if (context.state.features.support_shader_interlock
        && fragment_program_gxp.is_frag_color_used() != context.last_draw_was_framebuffer_fetch) {
        // restart the render pass to act as a barrier
        context.render_cmd.endRenderPass();

        if (fragment_program_gxp.is_frag_color_used()) {
            context.curr_renderpass_info.framebuffer = context.current_shader_interlock_framebuffer;
            context.curr_renderpass_info.renderPass = context.current_shader_interlock_pass;
        } else {
            context.curr_renderpass_info.framebuffer = context.current_framebuffer;
            context.curr_renderpass_info.renderPass = context.current_render_pass;
        }

        context.render_cmd.beginRenderPass(context.curr_renderpass_info, vk::SubpassContents::eInline);
        context.last_draw_was_framebuffer_fetch = fragment_program_gxp.is_frag_color_used();
    }

    if (context.current_visibility_buffer != nullptr && context.current_query_idx != -1 && !context.is_in_query) {
        if (context.current_visibility_buffer->queries_used[context.current_query_idx]) {
            LOG_WARN_ONCE("Visibility buffer entry is used more than once in a scene");
            // still let this happen, this is a validation error but I think most GPUs should be fine with it
        }
        context.current_visibility_buffer->queries_used[context.current_query_idx] = true;

        context.visibility_max_used_idx = std::max(context.visibility_max_used_idx, context.current_query_idx);

        const vk::QueryControlFlags control_flags = (context.is_query_op_increment && context.state.physical_device_features.occlusionQueryPrecise) ? vk::QueryControlFlagBits::ePrecise : vk::QueryControlFlags();
        context.render_cmd.beginQuery(context.current_visibility_buffer->query_pool, context.current_query_idx, control_flags);
        context.is_in_query = true;
    }

    // do we need to check for a pipeline change?
    if (context.refresh_pipeline || type != context.last_primitive) {
        context.refresh_pipeline = false;
        context.last_primitive = type;

        // We don't want to defer cases where we draw a whole quad over the screen as these draws could be necessary
        // to be able to see anything
        bool can_be_whole_quad = instance_count == 1 && count <= 6;
        vk::Pipeline new_pipeline = context.state.pipeline_cache.retrieve_pipeline(context, type, !can_be_whole_quad, mem);

        if (mg2_clear_trace) {
            std::fprintf(stderr,
                "VITA3K_MG2_CLEAR_PIPELINE call=%u new=%p old=%p whole_quad=%u\n",
                mg2_clear_call, VkPipeline(new_pipeline), VkPipeline(context.current_pipeline),
                unsigned(can_be_whole_quad));
            std::fflush(stderr);
        }

        if (new_pipeline != context.current_pipeline) {
            context.current_pipeline = new_pipeline;

            if (new_pipeline != nullptr)
                context.render_cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, context.current_pipeline);
        }
    }

    // can happen with asynchronous pipeline compilation
    if (context.current_pipeline == nullptr) {
        if (mg2_clear_trace) {
            std::fprintf(stderr, "VITA3K_MG2_CLEAR_GATE call=%u gate=no_pipeline\n", mg2_clear_call);
            std::fflush(stderr);
        }
        return;
    }

    // A context whose viewport was never set has a zero-sized one, which
    // Vulkan rejects; rasterize over the whole render target instead.
    if (context.viewport.width == 0.0f || context.viewport.height == 0.0f) {
        context.viewport = vk::Viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(context.render_target->width),
            .height = static_cast<float>(context.render_target->height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f
        };
        context.render_cmd.setViewport(0, context.viewport);
        LOG_WARN_ONCE("GXM viewport not initialized; using the full render target for rasterization");
    }

    // update uniforms if needed
    // first update the buffer and texture count
    auto &vert_render_data = context.record.vertex_program.get(mem)->renderer_data;
    auto &frag_render_data = context.record.fragment_program.get(mem)->renderer_data;

    if (use_memory_mapping) {
        context.curr_vert_ublock.set_buffer_count(vert_render_data->buffer_count);
        context.curr_frag_ublock.set_buffer_count(frag_render_data->buffer_count);
    }

    if (context.state.features.use_texture_viewport) {
        context.curr_vert_ublock.set_texture_count(vert_render_data->texture_count);
        context.curr_frag_ublock.set_texture_count(frag_render_data->texture_count);
    }

    auto &vert_ublock = context.curr_vert_ublock.base_block;
    vert_ublock.viewport_flip = context.record.viewport_flip;
    vert_ublock.viewport_flag = (context.record.viewport_flat) ? 0.0f : 1.0f;
    const char *const gl_clip_compat_env = std::getenv("VITA3K_ACXI_GL_CLIP_COMPAT");
    const char *const force_clip_reject_env = std::getenv("VITA3K_ACXI_FORCE_CLIP_REJECT");
    const bool gl_clip_compat = config.run_app_path && *config.run_app_path == "ACXI00001"
        && gl_clip_compat_env && gl_clip_compat_env[0] == '1' && gl_clip_compat_env[1] == 0;
    const bool force_clip_reject = config.run_app_path && *config.run_app_path == "ACXI00001"
        && force_clip_reject_env && force_clip_reject_env[0] == '1' && force_clip_reject_env[1] == 0;
    const int force_clip_plane = acxi_force_clip_plane(config, vert_render_data->hash);
    vert_ublock.w_clamp_enabled = gl_clip_compat ? 0.0f : context.record.w_clamp_enabled ? 1.0f : 0.0f;
    vert_ublock.w_clamp_value = context.record.w_clamp_value;
    vert_ublock.post_viewport_clip = force_clip_reject ? 2.0f
        : force_clip_plane >= 0 ? 3.0f + static_cast<float>(force_clip_plane)
                                : gl_clip_compat ? 1.0f : 0.0f;
    vert_ublock.z_offset = context.record.z_offset;
    vert_ublock.z_scale = context.record.z_scale;
    vert_ublock.screen_width = context.render_target->width / context.state.res_multiplier;
    vert_ublock.screen_height = context.render_target->height / context.state.res_multiplier;

    static uint32_t doubledragon_draw_state_trace_count = 0;
    const char *const doubledragon_viewport_trace = std::getenv("VITA3K_DDRG_VIEWPORT_TRACE");
    if (doubledragon_viewport_trace && doubledragon_viewport_trace[0] == '1'
        && doubledragon_viewport_trace[1] == 0
        && doubledragon_draw_state_trace_count < 256) {
        uint64_t vertex_hash_prefix = 0;
        std::memcpy(&vertex_hash_prefix, vert_render_data->hash.data(), sizeof(vertex_hash_prefix));
        std::fprintf(stderr,
            "VITA3K_DDRG_DRAW_STATE call=%u count=%zu instances=%u viewport=%g,%g,%g,%g,%g,%g scissor=%d,%d,%u,%u flat=%u flip=%g,%g,%g,%g z=%g,%g screen=%g,%g target=%ux%u vertex_hash=%016llX\n",
            ++doubledragon_draw_state_trace_count, count, instance_count,
            context.viewport.x, context.viewport.y, context.viewport.width, context.viewport.height,
            context.viewport.minDepth, context.viewport.maxDepth,
            context.scissor.offset.x, context.scissor.offset.y,
            context.scissor.extent.width, context.scissor.extent.height,
            static_cast<unsigned>(context.record.viewport_flat),
            context.record.viewport_flip[0], context.record.viewport_flip[1],
            context.record.viewport_flip[2], context.record.viewport_flip[3],
            context.record.z_offset, context.record.z_scale,
            vert_ublock.screen_width, vert_ublock.screen_height,
            context.render_target->width, context.render_target->height,
            static_cast<unsigned long long>(vertex_hash_prefix));
        std::fflush(stderr);
    }

    if (context.curr_vert_ublock.changed || memcmp(&context.prev_vert_ublock, &vert_ublock, sizeof(vert_ublock)) != 0) {
        // TODO: this intermediate step can be avoided
        context.curr_vert_ublock.copy_to(context.shader_info_temp);
        context.vertex_info_uniform_buffer.allocate(context.prerender_cmd, context.curr_vert_ublock.get_size(), context.shader_info_temp);
        memcpy(&context.prev_vert_ublock, &vert_ublock, sizeof(vert_ublock));
    }

    auto &frag_ublock = context.curr_frag_ublock.base_block;
    frag_ublock.writing_mask = context.record.writing_mask;
    frag_ublock.res_multiplier = context.state.res_multiplier;
    const bool has_msaa = context.render_target->multisample_mode;
    const bool has_downscale = context.record.color_surface.downscale;
    if (has_msaa && !has_downscale)
        frag_ublock.res_multiplier *= 2;
    else if (!has_msaa && has_downscale)
        frag_ublock.res_multiplier /= 2;

    if (context.curr_frag_ublock.changed || memcmp(&context.prev_frag_ublock, &frag_ublock, sizeof(frag_ublock)) != 0) {
        // TODO: this intermediate step can be avoided
        context.curr_frag_ublock.copy_to(context.shader_info_temp);
        context.fragment_info_uniform_buffer.allocate(context.prerender_cmd, context.curr_frag_ublock.get_size(), context.shader_info_temp);
        memcpy(&context.prev_frag_ublock, &frag_ublock, sizeof(frag_ublock));
    }

    // create, update and bind descriptors (uniforms and textures)
    draw_bind_descriptors(context, mem);
    static uint32_t nba2k16_crowd_uniform_draw_count = 0;
    const bool nba2k16_crowd_draw_trace = !use_memory_mapping
        && diagnostics::nba2k16_crowd_uniform_trace_enabled(
            vert_render_data->hash.data(), vert_render_data->hash.size())
        && nba2k16_crowd_uniform_draw_count < diagnostics::NBA2K16_CROWD_UNIFORM_TRACE_LIMIT;
    if (nba2k16_crowd_draw_trace) {
        const uint32_t trace_call = ++nba2k16_crowd_uniform_draw_count;
        const uint32_t ring_base = context.vertex_uniform_stream_ring_buffer.data_offset;
        const uint32_t total_bytes = vert_render_data->max_total_uniform_buffer_storage * 4u;
        const uint8_t *const ring_bytes = context.vertex_uniform_stream_ring_buffer.mapped_at(ring_base);
        const bool ring_readable = ring_bytes
            && total_bytes >= diagnostics::NBA2K16_CROWD_UNIFORM_BYTES
            && uint64_t(ring_base) + diagnostics::NBA2K16_CROWD_UNIFORM_BYTES
                <= context.vertex_uniform_stream_ring_buffer.mapped_size();
        const auto ring_hashes = ring_readable
            ? diagnostics::nba2k16_crowd_uniform_hashes(ring_bytes, diagnostics::NBA2K16_CROWD_UNIFORM_BYTES)
            : diagnostics::Nba2k16CrowdUniformHashes{};
        std::fprintf(stderr,
            "VITA3K_NBA2K16_CROWD_UB_DRAW call=%u frame=%llu scene=%llu count=%zu instances=%u base=%u total=%u readable=%u ring=%016llX ring_instance=%016llX ring_matrix=%016llX\n",
            trace_call, static_cast<unsigned long long>(context.frame_timestamp),
            static_cast<unsigned long long>(context.scene_timestamp), count, instance_count,
            ring_base, total_bytes, static_cast<unsigned>(ring_readable),
            static_cast<unsigned long long>(ring_hashes.full),
            static_cast<unsigned long long>(ring_hashes.instance),
            static_cast<unsigned long long>(ring_hashes.matrix));
        std::fflush(stderr);
    }
    if (mg2_clear_trace) {
        const size_t vertex_bytes_count = std::min<size_t>(mg2_vertex_renderer->max_total_uniform_buffer_storage * 4u, 64u);
        const size_t fragment_bytes_count = std::min<size_t>(mg2_fragment_renderer->max_total_uniform_buffer_storage * 4u, 64u);
        const uint32_t vertex_offset = context.vertex_uniform_stream_ring_buffer.data_offset;
        const uint32_t fragment_offset = context.fragment_uniform_stream_ring_buffer.data_offset;
        const uint8_t *const vertex_bytes = vertex_bytes_count
            ? context.vertex_uniform_stream_ring_buffer.mapped_at(vertex_offset)
            : nullptr;
        const uint8_t *const fragment_bytes = fragment_bytes_count
            ? context.fragment_uniform_stream_ring_buffer.mapped_at(fragment_offset)
            : nullptr;
        const bool vertex_readable = vertex_bytes
            && uint64_t(vertex_offset) + vertex_bytes_count <= context.vertex_uniform_stream_ring_buffer.mapped_size();
        const bool fragment_readable = fragment_bytes
            && uint64_t(fragment_offset) + fragment_bytes_count <= context.fragment_uniform_stream_ring_buffer.mapped_size();
        std::fprintf(stderr,
            "VITA3K_MG2_CLEAR_DESCRIPTORS call=%u pipeline=%p mapped=%u offsets=%u,%u,%u,%u allocated=%u,%u uniform_sizes=%u,%u readable=%u,%u vertex=",
            mg2_clear_call, VkPipeline(context.current_pipeline), unsigned(use_memory_mapping),
            context.vertex_info_uniform_buffer.data_offset,
            context.fragment_info_uniform_buffer.data_offset,
            vertex_offset, fragment_offset,
            unsigned(context.vertex_uniform_storage_allocated), unsigned(context.fragment_uniform_storage_allocated),
            unsigned(mg2_vertex_renderer->max_total_uniform_buffer_storage * 4u),
            unsigned(mg2_fragment_renderer->max_total_uniform_buffer_storage * 4u),
            unsigned(vertex_readable), unsigned(fragment_readable));
        minigore_clear_trace_bytes(vertex_readable ? vertex_bytes : nullptr, vertex_readable ? vertex_bytes_count : 0);
        std::fprintf(stderr, "VITA3K_MG2_CLEAR_FRAGMENT call=%u bytes=", mg2_clear_call);
        minigore_clear_trace_bytes(fragment_readable ? fragment_bytes : nullptr, fragment_readable ? fragment_bytes_count : 0);
        std::fflush(stderr);
    }

    const bool bounded_probe = capture_bios_draw(context, type, format, indices, count, instance_count, mem, config);
    if (config.log_active_shaders && !bounded_probe) {
        const std::string hash_text_f = hex_string(context.record.fragment_program.get(mem)->renderer_data->hash);
        const std::string hash_text_v = hex_string(context.record.vertex_program.get(mem)->renderer_data->hash);

        LOG_DEBUG("\nVertex  : {}\nFragment: {}", hash_text_v, hash_text_f);
        LOG_DEBUG("Vertex default uniform buffer: {}\n", spdlog::to_hex(context.ubo_data[0], 16));
        LOG_DEBUG("Fragment default uniform buffer: {}\n", spdlog::to_hex(context.ubo_data[SCE_GXM_REAL_MAX_UNIFORM_BUFFER], 16));
    }

    // Upload index data.
    vk::IndexType index_type = (format == SCE_GXM_INDEX_FORMAT_U16) ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    uint32_t max_index = 0;
    if (use_memory_mapping) {
        auto [buffer, offset] = context.state.get_matching_mapping(indices);
        if (context.state.mapping_method == MappingMethod::DoubleBuffer) {
            TrappedBuffer *trapped_buffer = context.state.buffer_trapping.access_buffer(indices.address(), count * index_size, mem);
            if (trapped_buffer->extra == ~0) {
                // store the max element in extra
                if (format == SCE_GXM_INDEX_FORMAT_U16) {
                    uint16_t *data = indices.cast<uint16_t>().get(mem);
                    trapped_buffer->extra = *std::max_element(&data[0], &data[count]);
                } else {
                    uint32_t *data = indices.cast<uint32_t>().get(mem);
                    trapped_buffer->extra = *std::max_element(&data[0], &data[count]);
                }
            }
            max_index = trapped_buffer->extra;
        }
        context.render_cmd.bindIndexBuffer(buffer, offset, index_type);
    } else {
        context.index_stream_ring_buffer.allocate(context.prerender_cmd, index_buffer_size, indices_ptr);
        context.render_cmd.bindIndexBuffer(context.index_stream_ring_buffer.handle(), context.index_stream_ring_buffer.data_offset, index_type);
    }

    // bind the vertex streams
    bind_vertex_streams(context, mem, instance_count, max_index);

    const bool acxi_skip_hangar_environment =
        acxi_skip_hangar_environment_draw(context, mem, config);
    const bool nba2k16_skip_draw =
        nba2k16_skip_draw_range(context, mem, config, type, count, instance_count);
    const char *const sixty_composite_trace_env = std::getenv("VITA3K_SIXTY_COMPOSITE_TRACE");
    const bool sixty_composite_trace = sixty_composite_trace_env
        && sixty_composite_trace_env[0] == '1' && sixty_composite_trace_env[1] == 0
        && hex_string(mg2_vertex_renderer->hash) == "1ed37f037294fd954438847fc53eb4eaa1602747c5f5e0262e68a210082f6353"
        && hex_string(mg2_fragment_renderer->hash) == "eca96fb5d9b62e549a89a583e7c738f1194db271ae81e919deec8845623e220b";
    if (sixty_composite_trace) {
        std::fprintf(stderr,
            "VITA3K_SIXTY_COMPOSITE_SUBMIT frame=%llu scene=%llu count=%zu instances=%u pipeline=%p framebuffer=%p color_view=%p color_data=0x%08X stride=%u viewport=%g,%g,%g,%g scissor=%d,%d,%u,%u\n",
            static_cast<unsigned long long>(context.frame_timestamp),
            static_cast<unsigned long long>(context.scene_timestamp), count, instance_count,
            static_cast<void *>(VkPipeline(context.current_pipeline)),
            static_cast<void *>(VkFramebuffer(context.current_framebuffer)),
            static_cast<void *>(VkImageView(context.current_color_view)),
            context.record.color_surface.data.address(), context.record.color_surface.strideInPixels,
            context.viewport.x, context.viewport.y, context.viewport.width, context.viewport.height,
            context.scissor.offset.x, context.scissor.offset.y,
            context.scissor.extent.width, context.scissor.extent.height);
        std::fflush(stderr);
    }
    if (!acxi_skip_hangar_environment && !nba2k16_skip_draw)
        context.render_cmd.drawIndexed(count, instance_count, 0, 0, 0);
    if (mg2_clear_trace) {
        std::fprintf(stderr, "VITA3K_MG2_CLEAR_SUBMIT call=%u count=%zu instances=%u\n",
            mg2_clear_call, count, instance_count);
        std::fflush(stderr);
    }
}

} // namespace renderer::vulkan
