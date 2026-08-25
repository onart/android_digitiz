#include "render/ScreenRenderer.hpp"

#include <vector>

#include <digitiz/core/log.hpp>

#include "render/Shader.hpp"

namespace digitiz::guest {

namespace {

// Four corners from the vertex id, as a triangle strip. The rectangle comes
// from a uniform, so there is nothing to rebuild when the view moves.
constexpr const char* kVertexSrc = R"(#version 300 es
uniform vec2 uViewport;   // surface pixels
uniform vec4 uRect;       // surface pixels, top-left origin: x, y, w, h

out vec2 vUv;

void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = c;
    vec2 p = uRect.xy + c * uRect.zw;
    gl_Position = vec4(p.x / uViewport.x * 2.0 - 1.0,
                       1.0 - p.y / uViewport.y * 2.0,
                       0.0, 1.0);
}
)";

constexpr const char* kFragmentSrc = R"(#version 300 es
precision mediump float;

uniform sampler2D uImage;

in vec2 vUv;
out vec4 fragColor;

void main() {
    fragColor = vec4(texture(uImage, vUv).rgb, 1.0);
}
)";

} // namespace

bool ScreenRenderer::init() {
    program_ = compile_program(kVertexSrc, kFragmentSrc);
    if (program_ == 0) {
        return false;
    }

    glGenVertexArrays(1, &vao_);

    u_viewport_ = glGetUniformLocation(program_, "uViewport");
    u_rect_ = glGetUniformLocation(program_, "uRect");
    u_image_ = glGetUniformLocation(program_, "uImage");
    return true;
}

void ScreenRenderer::release() {
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    tex_w_ = 0;
    tex_h_ = 0;
    painted_ = false;
}

bool ScreenRenderer::ensure_texture(int w, int h) {
    if (texture_ != 0 && tex_w_ == w && tex_h_ == h) {
        return true;
    }
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    // One level, and a filter that does not ask for the others: the default
    // minification filter wants mipmaps, and a texture that cannot supply them
    // is incomplete and samples as black.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Allocated from a zeroed buffer rather than left undefined. A batch is a
    // patch, not a picture, so the parts not covered yet are whatever the
    // texture was created with -- and an all-zero ETC2 block is very nearly
    // black, which passes for the background until the tiles land.
    const std::vector<std::uint8_t> empty(proto::etc2_size(w, h), 0);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_ETC2, w, h, 0,
                           static_cast<GLsizei>(empty.size()), empty.data());

    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        DZ_ERROR("screen: could not allocate a %dx%d ETC2 texture (GL 0x%04x)", w, h, err);
        glDeleteTextures(1, &texture_);
        texture_ = 0;
        tex_w_ = 0;
        tex_h_ = 0;
        return false;
    }

    tex_w_ = w;
    tex_h_ = h;
    painted_ = false;
    return true;
}

bool ScreenRenderer::upload(const FrameBatch& batch) {
    if (program_ == 0 || !batch.geometry.valid()) {
        return false;
    }
    if (!ensure_texture(batch.geometry.out_w, batch.geometry.out_h)) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texture_);

    std::size_t offset = 0;
    for (const std::uint16_t index : batch.tiles) {
        const core::Recti rect = batch.geometry.tile_output_rect(static_cast<int>(index));
        const std::size_t size = proto::etc2_size(rect.w, rect.h);
        if (size == 0 || offset + size > batch.blocks.size()) {
            // FrameReceiver already checked this; if it is wrong here the two
            // sides disagree about the geometry and the rest of the batch
            // would land in the wrong places.
            DZ_WARN("screen: tile %u does not fit the batch", index);
            return false;
        }
        glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h,
                                  GL_COMPRESSED_RGB8_ETC2, static_cast<GLsizei>(size),
                                  batch.blocks.data() + offset);
        offset += size;
    }

    region_ = batch.geometry.region;
    painted_ = true;
    return true;
}

void ScreenRenderer::draw(const core::ViewTransform& view, int surface_w, int surface_h) const {
    if (!has_image() || surface_w <= 0 || surface_h <= 0) {
        return;
    }

    // The encoded surface spans the region edge to edge, so the corners of one
    // are the corners of the other.
    const core::Vec2 tl = view.to_surface(core::Vec2{static_cast<double>(region_.x),
                                                     static_cast<double>(region_.y)});
    const core::Vec2 br = view.to_surface(
        core::Vec2{static_cast<double>(region_.x + region_.w),
                   static_cast<double>(region_.y + region_.h)});

    glUseProgram(program_);
    glBindVertexArray(vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glUniform1i(u_image_, 0);
    glUniform2f(u_viewport_, static_cast<float>(surface_w), static_cast<float>(surface_h));
    glUniform4f(u_rect_, static_cast<float>(tl.x), static_cast<float>(tl.y),
                static_cast<float>(br.x - tl.x), static_cast<float>(br.y - tl.y));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

} // namespace digitiz::guest
