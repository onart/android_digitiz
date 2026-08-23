#include "render/UiRenderer.hpp"

#include "render/Shader.hpp"

namespace digitiz::guest {

namespace {

// Four vertices as a triangle strip, corners derived from gl_VertexID.
// Precision qualifiers must match across stages for shared uniforms. The
// vertex stage defaults float to highp and the fragment stage to mediump, so
// anything declared in both is qualified explicitly.
constexpr const char* kVertexSrc = R"(#version 300 es
uniform highp vec2 uViewport;
uniform highp vec4 uRect;   // x, y, w, h  (top-left origin, surface px)

out highp vec2 vLocal;      // position within the rect, in pixels

void main() {
    vec2 uv = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vLocal = uv * uRect.zw;

    vec2 surface = uRect.xy + vLocal;
    vec2 ndc = vec2(surface.x / uViewport.x * 2.0 - 1.0,
                    1.0 - surface.y / uViewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

constexpr const char* kFragmentSrc = R"(#version 300 es
precision highp float;

uniform highp vec4  uRect;
uniform float uRadius;
uniform float uThickness;   // 0 fills the shape; otherwise an inward outline
uniform vec4  uColor;

in highp vec2 vLocal;
out vec4 fragColor;

void main() {
    vec2 half_size = uRect.zw * 0.5;
    vec2 q = abs(vLocal - half_size) - (half_size - uRadius);
    float sd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;

    // Inside the shape sd is negative, so an outline of thickness t is the
    // band -t <= sd <= 0: the outer edge fades in, the inner edge fades out.
    float alpha = 1.0 - smoothstep(-0.75, 0.75, sd);
    if (uThickness > 0.0) {
        alpha *= smoothstep(-uThickness - 0.75, -uThickness + 0.75, sd);
    }
    if (alpha <= 0.0) {
        discard;
    }
    fragColor = vec4(uColor.rgb, uColor.a * alpha);
}
)";

} // namespace

bool UiRenderer::init() {
    program_ = compile_program(kVertexSrc, kFragmentSrc);
    if (program_ == 0) {
        return false;
    }

    glGenVertexArrays(1, &vao_);

    u_viewport_ = glGetUniformLocation(program_, "uViewport");
    u_rect_ = glGetUniformLocation(program_, "uRect");
    u_radius_ = glGetUniformLocation(program_, "uRadius");
    u_thickness_ = glGetUniformLocation(program_, "uThickness");
    u_color_ = glGetUniformLocation(program_, "uColor");
    return true;
}

void UiRenderer::release() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

void UiRenderer::begin(int surface_w, int surface_h) {
    surface_w_ = surface_w;
    surface_h_ = surface_h;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(program_);
    glBindVertexArray(vao_);
    glUniform2f(u_viewport_, static_cast<float>(surface_w), static_cast<float>(surface_h));
}

void UiRenderer::end() {
    clear_clip();
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void UiRenderer::set_clip(Rect rect) {
    // glScissor counts from the bottom left; everything else here counts from
    // the top left.
    const GLint x = static_cast<GLint>(rect.x);
    const GLint y = static_cast<GLint>(static_cast<float>(surface_h_) - (rect.y + rect.h));
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, static_cast<GLsizei>(rect.w < 0.0f ? 0.0f : rect.w),
              static_cast<GLsizei>(rect.h < 0.0f ? 0.0f : rect.h));
}

void UiRenderer::clear_clip() {
    glDisable(GL_SCISSOR_TEST);
}

void UiRenderer::rounded_rect(Rect rect, float radius, Color fill) {
    draw_shape(rect, radius, 0.0f, fill);
}

void UiRenderer::rounded_rect_outline(Rect rect, float radius, float thickness, Color stroke) {
    draw_shape(rect, radius, thickness > 0.0f ? thickness : 1.0f, stroke);
}

void UiRenderer::draw_shape(Rect rect, float radius, float thickness, Color color) {
    if (program_ == 0 || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    const float limit = 0.5f * (rect.w < rect.h ? rect.w : rect.h);
    if (radius > limit) {
        radius = limit;
    }

    glUniform4f(u_rect_, rect.x, rect.y, rect.w, rect.h);
    glUniform1f(u_radius_, radius);
    glUniform1f(u_thickness_, thickness);
    glUniform4f(u_color_, color.r, color.g, color.b, color.a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

} // namespace digitiz::guest
