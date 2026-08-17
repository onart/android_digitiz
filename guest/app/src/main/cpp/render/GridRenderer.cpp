#include "render/GridRenderer.hpp"

#include "render/Shader.hpp"

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

// Standard fullscreen triangle: no vertex buffer, positions come from the id.
constexpr const char* kVertexSrc = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kFragmentSrc = R"(#version 300 es
precision highp float;

uniform vec2  uViewport;   // surface pixels
uniform float uScale;      // surface px per PC px
uniform vec2  uPan;        // PC px at the surface origin
uniform float uMinorStep;  // PC px between minor lines
uniform vec4  uDesktop;    // PC px: x, y, w, h
uniform vec3  uAccent;     // grid tint, shifts with host state

out vec4 fragColor;

// Distance in surface pixels to the nearest multiple of `step`.
float lineAlpha(float pc, float step, float halfWidth) {
    float f = pc / step;
    float d = abs(f - floor(f + 0.5)) * step * uScale;
    return 1.0 - smoothstep(halfWidth - 0.75, halfWidth + 0.75, d);
}

void main() {
    // gl_FragCoord has a bottom-left origin; the view transform is top-left.
    vec2 surface = vec2(gl_FragCoord.x, uViewport.y - gl_FragCoord.y);
    vec2 pc = surface / uScale + uPan;

    vec3 col = vec3(0.043, 0.047, 0.059);

    // Signed distance to the desktop rectangle, in surface pixels.
    vec2 half_size = uDesktop.zw * 0.5;
    vec2 center = uDesktop.xy + half_size;
    vec2 q = abs(pc - center) - half_size;
    float sd = (length(max(q, 0.0)) + min(max(q.x, q.y), 0.0)) * uScale;

    // The area that actually maps to the PC screen reads brighter.
    col = mix(col, vec3(0.078, 0.086, 0.105), 1.0 - smoothstep(-1.0, 1.0, sd));

    float minor = max(lineAlpha(pc.x, uMinorStep, 0.5),
                      lineAlpha(pc.y, uMinorStep, 0.5));
    float major = max(lineAlpha(pc.x, uMinorStep * 5.0, 0.85),
                      lineAlpha(pc.y, uMinorStep * 5.0, 0.85));

    col = mix(col, uAccent * 0.55, minor * 0.45);
    col = mix(col, uAccent, major * 0.7);

    // Desktop outline last so it stays readable over the grid.
    float border = 1.0 - smoothstep(1.0, 2.5, abs(sd));
    col = mix(col, uAccent * 1.35, border * 0.9);

    fragColor = vec4(col, 1.0);
}
)";

} // namespace

bool GridRenderer::init() {
    program_ = compile_program(kVertexSrc, kFragmentSrc);
    if (program_ == 0) {
        return false;
    }

    // ES 3.0 requires a bound VAO even when the draw uses no attributes.
    glGenVertexArrays(1, &vao_);

    u_viewport_ = glGetUniformLocation(program_, "uViewport");
    u_scale_ = glGetUniformLocation(program_, "uScale");
    u_pan_ = glGetUniformLocation(program_, "uPan");
    u_minor_step_ = glGetUniformLocation(program_, "uMinorStep");
    u_desktop_ = glGetUniformLocation(program_, "uDesktop");
    u_accent_ = glGetUniformLocation(program_, "uAccent");
    return true;
}

void GridRenderer::release() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

void GridRenderer::draw(const core::ViewTransform& view, int surface_w, int surface_h,
                        core::Recti desktop, bool injection_enabled, bool linked) {
    if (program_ == 0) {
        return;
    }

    glUseProgram(program_);
    glBindVertexArray(vao_);

    const double scale = view.scale();
    const core::Vec2 pan = view.pan();
    const double step = core::grid_step_pc(scale, 90.0);

    // Grey when there is no link, amber when linked but injection is off,
    // green when input is actually reaching the PC. Colour is the only status
    // display this screen has.
    float accent[3] = {0.30f, 0.33f, 0.39f};
    if (linked && injection_enabled) {
        accent[0] = 0.24f;
        accent[1] = 0.62f;
        accent[2] = 0.42f;
    } else if (linked) {
        accent[0] = 0.68f;
        accent[1] = 0.52f;
        accent[2] = 0.22f;
    }

    glUniform2f(u_viewport_, static_cast<float>(surface_w), static_cast<float>(surface_h));
    glUniform1f(u_scale_, static_cast<float>(scale));
    glUniform2f(u_pan_, static_cast<float>(pan.x), static_cast<float>(pan.y));
    glUniform1f(u_minor_step_, static_cast<float>(step));
    glUniform4f(u_desktop_, static_cast<float>(desktop.x), static_cast<float>(desktop.y),
                static_cast<float>(desktop.w), static_cast<float>(desktop.h));
    glUniform3fv(u_accent_, 1, accent);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

} // namespace digitiz::guest
