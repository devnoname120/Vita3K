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

#include <renderer/profile.h>

#include <renderer/gl/functions.h>
#include <renderer/gl/state.h>

#include <renderer/state.h>
#include <renderer/types.h>

#include <gxm/types.h>
#include <util/log.h>

#include <shader/spirv_recompiler.h>

#include <features/state.h>
#include <gxm/functions.h>

#include <spdlog/fmt/bin_to_hex.h>

#include <cstdlib>
#include <cstring>

namespace renderer::gl {
static bool nova3_gl_probe_enabled() {
    static const bool enabled = [] {
        const char *title = std::getenv("VITA3K_TTBR_REMAP_HLE_TITLE");
        return title && std::strcmp(title, "NOVA30001") == 0;
    }();
    return enabled;
}

static GLenum translate_primitive(SceGxmPrimitiveType primType) {
    R_PROFILE(__func__);

    switch (primType) {
    case SCE_GXM_PRIMITIVE_TRIANGLES:
        return GL_TRIANGLES;
    case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP:
        return GL_TRIANGLE_STRIP;
    case SCE_GXM_PRIMITIVE_TRIANGLE_FAN:
        return GL_TRIANGLE_FAN;
    case SCE_GXM_PRIMITIVE_LINES:
        return GL_LINES;
    case SCE_GXM_PRIMITIVE_POINTS:
        return GL_POINTS;
    case SCE_GXM_PRIMITIVE_TRIANGLE_EDGES: // Todo: Implement this
        LOG_WARN("Unsupported primitive: SCE_GXM_PRIMITIVE_TRIANGLE_EDGES, using GL_TRIANGLES instead");
        return GL_TRIANGLES;
    }
    return GL_TRIANGLES;
}

void draw(GLState &renderer, GLContext &context, const FeatureState &features, SceGxmPrimitiveType type, SceGxmIndexFormat format, void *indices, size_t count, uint32_t instance_count,
    MemState &mem, const Config &config) {
    R_PROFILE(__func__);

    GLuint program_id = context.last_draw_program;

    const SceGxmFragmentProgram &gxm_fragment_program = *context.record.fragment_program.get(mem);
    const SceGxmProgram &fragment_program_gxp = *gxm_fragment_program.program.get(mem);

    if (!renderer.features.use_mask_bit && gxm_fragment_program.is_maskupdate) {
        static bool has_happened = false;
        LOG_ERROR_IF(!has_happened, "Mask bit is disabled!");
        has_happened = true;
        return;
    }

    // Trying to cache: the last time vs this time shader pair. Does it different somehow?
    // If it's different, we need to switch. Else just stick to it.
    if (context.record.vertex_program.get(mem)->renderer_data->hash != context.last_draw_vertex_program_hash || context.record.fragment_program.get(mem)->renderer_data->hash != context.last_draw_fragment_program_hash) {
        // Need to recompile!
        SharedGLObject program = gl::compile_program(renderer, context, context.record, features, mem, config.shader_cache, config.spirv_shader, gxm_fragment_program.is_maskupdate);

        LOG_ERROR_IF(!program, "Fail to get program!");

        // Use it
        program_id = program ? (*program).get() : 0;
        context.last_draw_program = program_id;
    } else {
        program_id = context.last_draw_program;
    }

    if (config.log_active_shaders) {
        const std::string hash_text_f = hex_string(context.record.fragment_program.get(mem)->renderer_data->hash);
        const std::string hash_text_v = hex_string(context.record.vertex_program.get(mem)->renderer_data->hash);

        LOG_DEBUG("\nVertex  : {}\nFragment: {}", hash_text_v, hash_text_f);
        LOG_DEBUG(fmt::runtime("Vertex default uniform buffer: {}\n"), spdlog::to_hex(context.ubo_data[0], 16));
        LOG_DEBUG(fmt::runtime("Fragment default uniform buffer: {}\n"), spdlog::to_hex(context.ubo_data[SCE_GXM_REAL_MAX_UNIFORM_BUFFER], 16));
    }

    if (!program_id) {
        assert(false && "Should never happen");
        glGetIntegerv(GL_CURRENT_PROGRAM, reinterpret_cast<GLint *>(&program_id));
    }

    glUseProgram(program_id);

    const bool use_raw_image = renderer.features.preserve_f16_nan_as_u16 && color::is_write_surface_stored_rawly(gxm::get_base_format(context.record.color_surface.colorFormat));

    const SceGxmColorBaseFormat base_format = gxm::get_base_format(context.record.color_surface.colorFormat);
    const GLenum surface_format = color::translate_internal_format(base_format);

    if (fragment_program_gxp.is_frag_color_used() && features.is_programmable_blending_need_to_bind_color_attachment()) {
        if (use_raw_image) {
            glBindImageTexture(shader::COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE, context.current_color_attachment, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16UI);
            glBindImageTexture(shader::COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE, 0, 0, GL_FALSE, 0, GL_READ_WRITE, surface_format);
        } else {
            glBindImageTexture(shader::COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16UI);
            glBindImageTexture(shader::COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE, context.current_color_attachment, 0, GL_FALSE, 0, GL_READ_WRITE, surface_format);
        }
    }

    if (renderer.features.use_mask_bit)
        glBindImageTexture(shader::MASK_TEXTURE_SLOT_IMAGE, context.render_target->masktexture[0], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);

    shader::RenderVertUniformBlock &vert_ublock = context.current_vert_render_info;
    vert_ublock.viewport_flip = context.record.viewport_flip;
    vert_ublock.viewport_flag = (context.record.viewport_flat) ? 0.0f : 1.0f;
    const char *const gl_clip_compat_env = std::getenv("VITA3K_ACXI_GL_CLIP_COMPAT");
    const char *const force_clip_reject_env = std::getenv("VITA3K_ACXI_FORCE_CLIP_REJECT");
    const bool gl_clip_compat = config.run_app_path && *config.run_app_path == "ACXI00001"
        && gl_clip_compat_env && gl_clip_compat_env[0] == '1' && gl_clip_compat_env[1] == 0;
    const bool force_clip_reject = config.run_app_path && *config.run_app_path == "ACXI00001"
        && force_clip_reject_env && force_clip_reject_env[0] == '1' && force_clip_reject_env[1] == 0;
    vert_ublock.w_clamp_enabled = gl_clip_compat ? 0.0f : context.record.w_clamp_enabled ? 1.0f : 0.0f;
    vert_ublock.w_clamp_value = context.record.w_clamp_value;
    vert_ublock.post_viewport_clip = force_clip_reject ? 2.0f : gl_clip_compat ? 1.0f : 0.0f;
    vert_ublock.z_offset = context.record.z_offset;
    vert_ublock.z_scale = context.record.z_scale;
    vert_ublock.screen_width = static_cast<float>(context.record.color_surface.width);
    vert_ublock.screen_height = static_cast<float>(context.record.color_surface.height);

    if (memcmp(&context.previous_vert_info, &vert_ublock, sizeof(shader::RenderVertUniformBlock)) != 0) {
        std::pair<std::uint8_t *, std::size_t> allocated_buffer = context.vertex_info_uniform_buffer.allocate(sizeof(shader::RenderVertUniformBlock));
        std::memcpy(allocated_buffer.first, &vert_ublock, sizeof(shader::RenderVertUniformBlock));

        context.previous_vert_info = vert_ublock;

        glBindBufferRange(GL_UNIFORM_BUFFER, 2, context.vertex_info_uniform_buffer.handle(), allocated_buffer.second, sizeof(shader::RenderVertUniformBlock));
    }

    shader::RenderFragUniformBlock &frag_ublock = context.current_frag_render_info;
    const bool both_side_fragment_program_disabled = (context.record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED)
        && ((context.record.back_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED) || (context.record.two_sided == SCE_GXM_TWO_SIDED_DISABLED));
    if (both_side_fragment_program_disabled) {
        frag_ublock.front_disabled = 0.0f;
        frag_ublock.back_disabled = 0.0f;
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    } else {
        frag_ublock.front_disabled = (context.record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED) ? 1.0f : 0.0f;
        if (context.record.two_sided == SCE_GXM_TWO_SIDED_DISABLED)
            frag_ublock.back_disabled = frag_ublock.front_disabled;
        else
            frag_ublock.back_disabled = (context.record.back_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED) ? 1.0f : 0.0f;
    }

    if (context.record.is_maskupdate) {
        // Tests bypassed in maskupdate
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);

        glBindFramebuffer(GL_FRAMEBUFFER, context.render_target->maskbuffer[0]);
    }
    frag_ublock.writing_mask = context.record.writing_mask;
    frag_ublock.use_raw_image = static_cast<float>(use_raw_image);
    frag_ublock.res_multiplier = renderer.res_multiplier;
    const bool has_msaa = context.render_target->multisample_mode;
    const bool has_downscale = context.record.color_surface.downscale;
    if (has_msaa && !has_downscale)
        frag_ublock.res_multiplier *= 2;
    else if (!has_msaa && has_downscale)
        frag_ublock.res_multiplier /= 2;

    if (memcmp(&context.previous_frag_info, &frag_ublock, sizeof(shader::RenderFragUniformBlock)) != 0) {
        std::pair<std::uint8_t *, std::size_t> allocated_buffer = context.fragment_info_uniform_buffer.allocate(sizeof(shader::RenderFragUniformBlock));
        std::memcpy(allocated_buffer.first, &frag_ublock, sizeof(shader::RenderFragUniformBlock));

        context.previous_frag_info = frag_ublock;

        glBindBufferRange(GL_UNIFORM_BUFFER, 3, context.fragment_info_uniform_buffer.handle(), allocated_buffer.second, sizeof(shader::RenderFragUniformBlock));
    }

    // Upload vertex stream
    sync_vertex_streams_and_attributes(context, context.record, mem);

    // Upload index data.
    const GLsizeiptr index_size = (format == SCE_GXM_INDEX_FORMAT_U16) ? 2 : 4;
    const std::size_t index_buffer_size = index_size * count;

    std::pair<std::uint8_t *, std::size_t> index_gpu_ptr = context.index_stream_ring_buffer.allocate(index_buffer_size);
    if (!index_gpu_ptr.first) {
        LOG_ERROR("Failed to allocate index stream ring buffer data from GPU!");
        return;
    }

    std::memcpy(index_gpu_ptr.first, indices, index_buffer_size);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, context.index_stream_ring_buffer.handle());

    if (fragment_program_gxp.is_native_color()) {
        if (features.should_use_shader_interlock() && !config.spirv_shader) {
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        } else if (features.should_use_texture_barrier()) {
            // Needs texture barrier
            glTextureBarrier();
        }
    } else if (!context.self_sampling_indices.empty()) {
        if (features.support_texture_barrier) {
            glTextureBarrier();
        } else {
            glActiveTexture(GL_TEXTURE0 + context.self_sampling_indices[0]);
            std::uint64_t ping_pong = renderer.surface_cache.retrieve_ping_pong_color_surface_texture_handle(context.record.color_surface.data);
            if (ping_pong != 0) {
                for (std::size_t i = 0; i < context.self_sampling_indices.size(); i++) {
                    glActiveTexture(GL_TEXTURE0 + context.self_sampling_indices[i]);
                    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(ping_pong));
                }
            }
        }
    }

    // Draw.
    const GLenum mode = translate_primitive(type);
    const GLenum gl_type = format == SCE_GXM_INDEX_FORMAT_U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;

    static uint32_t nova3_probe_draw_count = 0;
    const bool nova3_probe_draw = nova3_gl_probe_enabled()
        && type == SCE_GXM_PRIMITIVE_TRIANGLE_FAN
        && count == 4
        && ++nova3_probe_draw_count <= 128;
    GLint probe_framebuffer = 0;
    GLenum probe_framebuffer_status = 0;
    if (nova3_probe_draw) {
        const GLenum prior_error = glGetError();
        GLint probe_program = 0;
        GLint probe_viewport[4] = {};
        GLint probe_scissor[4] = {};
        GLint probe_draw_buffer = 0;
        GLint probe_read_buffer = 0;
        GLint probe_sample_buffers = 0;
        GLint probe_samples = 0;
        GLboolean probe_color_mask[4] = {};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &probe_framebuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &probe_program);
        glGetIntegerv(GL_VIEWPORT, probe_viewport);
        glGetIntegerv(GL_SCISSOR_BOX, probe_scissor);
        glGetIntegerv(GL_DRAW_BUFFER, &probe_draw_buffer);
        glGetIntegerv(GL_READ_BUFFER, &probe_read_buffer);
        glGetIntegerv(GL_SAMPLE_BUFFERS, &probe_sample_buffers);
        glGetIntegerv(GL_SAMPLES, &probe_samples);
        glGetBooleanv(GL_COLOR_WRITEMASK, probe_color_mask);
        probe_framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        uint32_t index0 = 0;
        uint32_t index1 = 0;
        uint32_t index2 = 0;
        uint32_t index3 = 0;
        if (format == SCE_GXM_INDEX_FORMAT_U16) {
            const auto *data = static_cast<const uint16_t *>(indices);
            index0 = data[0];
            index1 = data[1];
            index2 = data[2];
            index3 = data[3];
        } else {
            const auto *data = static_cast<const uint32_t *>(indices);
            index0 = data[0];
            index1 = data[1];
            index2 = data[2];
            index3 = data[3];
        }
        const auto *fragment = reinterpret_cast<const GLFragmentProgram *>(gxm_fragment_program.renderer_data.get());
        LOG_WARN("NOVA3_GL_DRAW_BEFORE index={} framebuffer={} status=0x{:X} attachment={} program={} mode=0x{:X} gl_type=0x{:X} indices={},{},{},{} viewport={},{},{},{} scissor_enabled={} scissor={},{},{},{} color_mask={},{},{},{} draw_buffer=0x{:X} read_buffer=0x{:X} sample_buffers={} samples={} blend={} depth={} stencil={} cull={} prior_error=0x{:X} surface=0x{:08X} size={}x{} stride={} rt={}x{} msaa={} front_enable={} back_enable={} two_sided={} region_mode={} region={},{},{},{} fragment_mask={},{},{},{} native_color={} frag_color_used={}",
            nova3_probe_draw_count, probe_framebuffer, probe_framebuffer_status, context.current_color_attachment, probe_program, mode, gl_type,
            index0, index1, index2, index3,
            probe_viewport[0], probe_viewport[1], probe_viewport[2], probe_viewport[3],
            glIsEnabled(GL_SCISSOR_TEST), probe_scissor[0], probe_scissor[1], probe_scissor[2], probe_scissor[3],
            probe_color_mask[0], probe_color_mask[1], probe_color_mask[2], probe_color_mask[3],
            probe_draw_buffer, probe_read_buffer, probe_sample_buffers, probe_samples,
            glIsEnabled(GL_BLEND), glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_CULL_FACE), prior_error,
            context.record.color_surface.data.address(), context.record.color_surface.width, context.record.color_surface.height, context.record.color_surface.strideInPixels,
            context.render_target->width, context.render_target->height, static_cast<uint32_t>(context.render_target->multisample_mode),
            static_cast<uint32_t>(context.record.front_side_fragment_program_mode), static_cast<uint32_t>(context.record.back_side_fragment_program_mode),
            static_cast<uint32_t>(context.record.two_sided), static_cast<uint32_t>(context.record.region_clip_mode),
            context.record.region_clip_min.x, context.record.region_clip_min.y, context.record.region_clip_max.x, context.record.region_clip_max.y,
            fragment->color_mask_red, fragment->color_mask_green, fragment->color_mask_blue, fragment->color_mask_alpha,
            fragment_program_gxp.is_native_color(), fragment_program_gxp.is_frag_color_used());
    }

    if (instance_count == 1) {
        glDrawElements(mode, static_cast<GLsizei>(count), gl_type, reinterpret_cast<const void *>(index_gpu_ptr.second));
    } else {
        glDrawElementsInstanced(mode, static_cast<GLsizei>(count), gl_type, reinterpret_cast<const void *>(index_gpu_ptr.second), instance_count);
    }

    if (nova3_probe_draw) {
        const GLenum draw_error = glGetError();
        GLint pack_buffer = 0;
        GLint pack_row_length = 0;
        GLint pack_alignment = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
        uint32_t pixel = 0;
        GLenum read_error = 0;
        if (pack_buffer == 0 && probe_framebuffer_status == GL_FRAMEBUFFER_COMPLETE) {
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(static_cast<GLint>(context.record.color_surface.width / 2),
                static_cast<GLint>(context.record.color_surface.height / 2),
                1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
            read_error = glGetError();
            glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
            glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
        }
        LOG_WARN("NOVA3_GL_DRAW_AFTER index={} framebuffer={} draw_error=0x{:X} read_error=0x{:X} pack_buffer={} center=0x{:08X}",
            nova3_probe_draw_count, probe_framebuffer, draw_error, read_error, pack_buffer, pixel);
    }

    // Restore context for normal draws
    if (context.record.is_maskupdate) {
        sync_depth_data(context.record);
        sync_stencil_data(context.record, mem);
        glBindFramebuffer(GL_FRAMEBUFFER, context.current_framebuffer);
    }

    if (both_side_fragment_program_disabled) {
        sync_blending(context.record, mem);
    }

    context.last_draw_vertex_program_hash = context.record.vertex_program.get(mem)->renderer_data->hash;
    context.last_draw_fragment_program_hash = context.record.fragment_program.get(mem)->renderer_data->hash;

    context.vertex_stream_ring_buffer.draw_call_done();
    context.index_stream_ring_buffer.draw_call_done();
    context.vertex_uniform_stream_ring_buffer.draw_call_done();
    context.fragment_uniform_stream_ring_buffer.draw_call_done();
    context.vertex_info_uniform_buffer.draw_call_done();
    context.fragment_info_uniform_buffer.draw_call_done();

    clear_previous_uniform_storage(context);
}
} // namespace renderer::gl
