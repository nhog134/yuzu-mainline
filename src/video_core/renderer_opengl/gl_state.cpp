// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <iterator>
#include <glad/glad.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "video_core/renderer_opengl/gl_state.h"

MICROPROFILE_DEFINE(OpenGL_State, "OpenGL", "State Change", MP_RGB(192, 128, 128));

namespace OpenGL {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

OpenGLState OpenGLState::cur_state;

namespace {

template <typename T>
bool UpdateValue(T& current_value, const T new_value) {
    const bool changed = current_value != new_value;
    current_value = new_value;
    return changed;
}

template <typename T1, typename T2>
bool UpdateTie(T1 current_value, const T2 new_value) {
    const bool changed = current_value != new_value;
    current_value = new_value;
    return changed;
}

template <typename T>
std::optional<std::pair<GLuint, GLsizei>> UpdateArray(T& current_values, const T& new_values) {
    std::optional<std::size_t> first;
    std::size_t last;
    for (std::size_t i = 0; i < std::size(current_values); ++i) {
        if (!UpdateValue(current_values[i], new_values[i])) {
            continue;
        }
        if (!first) {
            first = i;
        }
        last = i;
    }
    if (!first) {
        return std::nullopt;
    }
    return std::make_pair(static_cast<GLuint>(*first), static_cast<GLsizei>(last - *first + 1));
}

void Enable(GLenum cap, bool enable) {
    if (enable) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

void Enable(GLenum cap, GLuint index, bool enable) {
    if (enable) {
        glEnablei(cap, index);
    } else {
        glDisablei(cap, index);
    }
}

void Enable(GLenum cap, bool& current_value, bool new_value) {
    if (UpdateValue(current_value, new_value)) {
        Enable(cap, new_value);
    }
}

void Enable(GLenum cap, GLuint index, bool& current_value, bool new_value) {
    if (UpdateValue(current_value, new_value)) {
        Enable(cap, index, new_value);
    }
}

} // Anonymous namespace

OpenGLState::OpenGLState() = default;

void OpenGLState::SetDefaultViewports() {
    viewports.fill(Viewport{});
}

void OpenGLState::ApplyFramebufferState() {
    if (UpdateValue(cur_state.draw.read_framebuffer, draw.read_framebuffer)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, draw.read_framebuffer);
    }
    if (UpdateValue(cur_state.draw.draw_framebuffer, draw.draw_framebuffer)) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw.draw_framebuffer);
    }
}

void OpenGLState::ApplyShaderProgram() {
    if (UpdateValue(cur_state.draw.shader_program, draw.shader_program)) {
        glUseProgram(draw.shader_program);
    }
}

void OpenGLState::ApplyProgramPipeline() {
    if (UpdateValue(cur_state.draw.program_pipeline, draw.program_pipeline)) {
        glBindProgramPipeline(draw.program_pipeline);
    }
}

void OpenGLState::ApplyClipDistances() {
    for (std::size_t i = 0; i < clip_distance.size(); ++i) {
        Enable(GL_CLIP_DISTANCE0 + static_cast<GLenum>(i), cur_state.clip_distance[i],
               clip_distance[i]);
    }
}

void OpenGLState::ApplyFragmentColorClamp() {
    if (UpdateValue(cur_state.fragment_color_clamp.enabled, fragment_color_clamp.enabled)) {
        glClampColor(GL_CLAMP_FRAGMENT_COLOR_ARB,
                     fragment_color_clamp.enabled ? GL_TRUE : GL_FALSE);
    }
}

void OpenGLState::ApplyMultisample() {
    Enable(GL_SAMPLE_ALPHA_TO_COVERAGE, cur_state.multisample_control.alpha_to_coverage,
           multisample_control.alpha_to_coverage);
    Enable(GL_SAMPLE_ALPHA_TO_ONE, cur_state.multisample_control.alpha_to_one,
           multisample_control.alpha_to_one);
}

void OpenGLState::ApplyRasterizerDiscard() {
    Enable(GL_RASTERIZER_DISCARD, cur_state.rasterizer_discard, rasterizer_discard);
}

void OpenGLState::ApplyColorMask() {
    for (std::size_t i = 0; i < Maxwell::NumRenderTargets; ++i) {
        const auto& updated = color_mask[i];
        auto& current = cur_state.color_mask[i];
        if (updated.red_enabled != current.red_enabled ||
            updated.green_enabled != current.green_enabled ||
            updated.blue_enabled != current.blue_enabled ||
            updated.alpha_enabled != current.alpha_enabled) {
            current = updated;
            glColorMaski(static_cast<GLuint>(i), updated.red_enabled, updated.green_enabled,
                         updated.blue_enabled, updated.alpha_enabled);
        }
    }
}

void OpenGLState::ApplyStencilTest() {
    Enable(GL_STENCIL_TEST, cur_state.stencil.test_enabled, stencil.test_enabled);

    const auto ConfigStencil = [](GLenum face, const auto& config, auto& current) {
        if (current.test_func != config.test_func || current.test_ref != config.test_ref ||
            current.test_mask != config.test_mask) {
            current.test_func = config.test_func;
            current.test_ref = config.test_ref;
            current.test_mask = config.test_mask;
            glStencilFuncSeparate(face, config.test_func, config.test_ref, config.test_mask);
        }
        if (current.action_depth_fail != config.action_depth_fail ||
            current.action_depth_pass != config.action_depth_pass ||
            current.action_stencil_fail != config.action_stencil_fail) {
            current.action_depth_fail = config.action_depth_fail;
            current.action_depth_pass = config.action_depth_pass;
            current.action_stencil_fail = config.action_stencil_fail;
            glStencilOpSeparate(face, config.action_stencil_fail, config.action_depth_fail,
                                config.action_depth_pass);
        }
        if (current.write_mask != config.write_mask) {
            current.write_mask = config.write_mask;
            glStencilMaskSeparate(face, config.write_mask);
        }
    };
    ConfigStencil(GL_FRONT, stencil.front, cur_state.stencil.front);
    ConfigStencil(GL_BACK, stencil.back, cur_state.stencil.back);
}

void OpenGLState::ApplyViewport() {
    for (GLuint i = 0; i < static_cast<GLuint>(Maxwell::NumViewports); ++i) {
        const auto& updated = viewports[i];
        auto& current = cur_state.viewports[i];

        if (current.x != updated.x || current.y != updated.y || current.width != updated.width ||
            current.height != updated.height) {
            current.x = updated.x;
            current.y = updated.y;
            current.width = updated.width;
            current.height = updated.height;
            glViewportIndexedf(i, static_cast<GLfloat>(updated.x), static_cast<GLfloat>(updated.y),
                               static_cast<GLfloat>(updated.width),
                               static_cast<GLfloat>(updated.height));
        }
        if (current.depth_range_near != updated.depth_range_near ||
            current.depth_range_far != updated.depth_range_far) {
            current.depth_range_near = updated.depth_range_near;
            current.depth_range_far = updated.depth_range_far;
            glDepthRangeIndexed(i, updated.depth_range_near, updated.depth_range_far);
        }

        Enable(GL_SCISSOR_TEST, i, current.scissor.enabled, updated.scissor.enabled);

        if (current.scissor.x != updated.scissor.x || current.scissor.y != updated.scissor.y ||
            current.scissor.width != updated.scissor.width ||
            current.scissor.height != updated.scissor.height) {
            current.scissor.x = updated.scissor.x;
            current.scissor.y = updated.scissor.y;
            current.scissor.width = updated.scissor.width;
            current.scissor.height = updated.scissor.height;
            glScissorIndexed(i, updated.scissor.x, updated.scissor.y, updated.scissor.width,
                             updated.scissor.height);
        }
    }
}

void OpenGLState::ApplyGlobalBlending() {
    const Blend& updated = blend[0];
    Blend& current = cur_state.blend[0];

    Enable(GL_BLEND, current.enabled, updated.enabled);

    if (current.src_rgb_func != updated.src_rgb_func ||
        current.dst_rgb_func != updated.dst_rgb_func || current.src_a_func != updated.src_a_func ||
        current.dst_a_func != updated.dst_a_func) {
        current.src_rgb_func = updated.src_rgb_func;
        current.dst_rgb_func = updated.dst_rgb_func;
        current.src_a_func = updated.src_a_func;
        current.dst_a_func = updated.dst_a_func;
        glBlendFuncSeparate(updated.src_rgb_func, updated.dst_rgb_func, updated.src_a_func,
                            updated.dst_a_func);
    }

    if (current.rgb_equation != updated.rgb_equation || current.a_equation != updated.a_equation) {
        current.rgb_equation = updated.rgb_equation;
        current.a_equation = updated.a_equation;
        glBlendEquationSeparate(updated.rgb_equation, updated.a_equation);
    }
}

void OpenGLState::ApplyTargetBlending(std::size_t target, bool force) {
    const Blend& updated = blend[target];
    Blend& current = cur_state.blend[target];

    if (current.enabled != updated.enabled || force) {
        current.enabled = updated.enabled;
        Enable(GL_BLEND, static_cast<GLuint>(target), updated.enabled);
    }

    if (UpdateTie(std::tie(current.src_rgb_func, current.dst_rgb_func, current.src_a_func,
                           current.dst_a_func),
                  std::tie(updated.src_rgb_func, updated.dst_rgb_func, updated.src_a_func,
                           updated.dst_a_func))) {
        glBlendFuncSeparatei(static_cast<GLuint>(target), updated.src_rgb_func,
                             updated.dst_rgb_func, updated.src_a_func, updated.dst_a_func);
    }

    if (UpdateTie(std::tie(current.rgb_equation, current.a_equation),
                  std::tie(updated.rgb_equation, updated.a_equation))) {
        glBlendEquationSeparatei(static_cast<GLuint>(target), updated.rgb_equation,
                                 updated.a_equation);
    }
}

void OpenGLState::ApplyBlending() {
    if (independant_blend.enabled) {
        const bool force = independant_blend.enabled != cur_state.independant_blend.enabled;
        for (std::size_t target = 0; target < Maxwell::NumRenderTargets; ++target) {
            ApplyTargetBlending(target, force);
        }
    } else {
        ApplyGlobalBlending();
    }
    cur_state.independant_blend.enabled = independant_blend.enabled;
}

void OpenGLState::ApplyClipControl() {
    if (UpdateTie(std::tie(cur_state.clip_control.origin, cur_state.clip_control.depth_mode),
                  std::tie(clip_control.origin, clip_control.depth_mode))) {
        glClipControl(clip_control.origin, clip_control.depth_mode);
    }
}

void OpenGLState::ApplyRenderBuffer() {
    if (cur_state.renderbuffer != renderbuffer) {
        cur_state.renderbuffer = renderbuffer;
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    }
}

void OpenGLState::ApplyTextures() {
    const std::size_t size = std::size(textures);
    for (std::size_t i = 0; i < size; ++i) {
        if (UpdateValue(cur_state.textures[i], textures[i])) {
            // BindTextureUnit doesn't support binding null textures, skip those binds.
            // TODO(Rodrigo): Stop using null textures
            if (textures[i] != 0) {
                glBindTextureUnit(static_cast<GLuint>(i), textures[i]);
            }
        }
    }
}

void OpenGLState::ApplySamplers() {
    const std::size_t size = std::size(samplers);
    for (std::size_t i = 0; i < size; ++i) {
        if (UpdateValue(cur_state.samplers[i], samplers[i])) {
            glBindSampler(static_cast<GLuint>(i), samplers[i]);
        }
    }
}

void OpenGLState::ApplyImages() {
    if (const auto update = UpdateArray(cur_state.images, images)) {
        glBindImageTextures(update->first, update->second, images.data() + update->first);
    }
}

void OpenGLState::Apply() {
    MICROPROFILE_SCOPE(OpenGL_State);
    ApplyFramebufferState();
    ApplyShaderProgram();
    ApplyProgramPipeline();
    ApplyClipDistances();
    ApplyFragmentColorClamp();
    ApplyMultisample();
    ApplyRasterizerDiscard();
    ApplyColorMask();
    ApplyViewport();
    ApplyStencilTest();
    ApplyBlending();
    ApplyTextures();
    ApplySamplers();
    ApplyImages();
    ApplyClipControl();
    ApplyRenderBuffer();
}

void OpenGLState::EmulateViewportWithScissor() {
    auto& current = viewports[0];
    if (current.scissor.enabled) {
        const GLint left = std::max(current.x, current.scissor.x);
        const GLint right =
            std::max(current.x + current.width, current.scissor.x + current.scissor.width);
        const GLint bottom = std::max(current.y, current.scissor.y);
        const GLint top =
            std::max(current.y + current.height, current.scissor.y + current.scissor.height);
        current.scissor.x = std::max(left, 0);
        current.scissor.y = std::max(bottom, 0);
        current.scissor.width = std::max(right - left, 0);
        current.scissor.height = std::max(top - bottom, 0);
    } else {
        current.scissor.enabled = true;
        current.scissor.x = current.x;
        current.scissor.y = current.y;
        current.scissor.width = current.width;
        current.scissor.height = current.height;
    }
}

OpenGLState& OpenGLState::UnbindTexture(GLuint handle) {
    for (auto& texture : textures) {
        if (texture == handle) {
            texture = 0;
        }
    }
    return *this;
}

OpenGLState& OpenGLState::ResetSampler(GLuint handle) {
    for (auto& sampler : samplers) {
        if (sampler == handle) {
            sampler = 0;
        }
    }
    return *this;
}

OpenGLState& OpenGLState::ResetProgram(GLuint handle) {
    if (draw.shader_program == handle) {
        draw.shader_program = 0;
    }
    return *this;
}

OpenGLState& OpenGLState::ResetPipeline(GLuint handle) {
    if (draw.program_pipeline == handle) {
        draw.program_pipeline = 0;
    }
    return *this;
}

OpenGLState& OpenGLState::ResetFramebuffer(GLuint handle) {
    if (draw.read_framebuffer == handle) {
        draw.read_framebuffer = 0;
    }
    if (draw.draw_framebuffer == handle) {
        draw.draw_framebuffer = 0;
    }
    return *this;
}

OpenGLState& OpenGLState::ResetRenderbuffer(GLuint handle) {
    if (renderbuffer == handle) {
        renderbuffer = 0;
    }
    return *this;
}

} // namespace OpenGL
