#include "render/GridRenderer.hpp"

#include <algorithm>

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

#define MAX_MONITORS 8

uniform vec2  uViewport;                  // surface pixels
uniform float uScale;                     // surface px per PC px
uniform vec2  uPan;                       // PC px at the surface origin
uniform float uMinorStep;                 // PC px between minor lines
uniform int   uMonitorCount;
uniform vec4  uMonitors[MAX_MONITORS];    // PC px: x, y, w, h
uniform vec3  uAccent;                    // grid tint, shifts with host state

out vec4 fragColor;

// Distance in surface pixels to the nearest multiple of `step`.
float lineAlpha(float pc, float step, float halfWidth) {
    float f = pc / step;
    float d = abs(f - floor(f + 0.5)) * step * uScale;
    return 1.0 - smoothstep(halfWidth - 0.75, halfWidth + 0.75, d);
}

float sdRect(vec2 p, vec4 r) {
    vec2 half_size = r.zw * 0.5;
    vec2 q = abs(p - (r.xy + half_size)) - half_size;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

void main() {
    // gl_FragCoord has a bottom-left origin; the view transform is top-left.
    vec2 surface = vec2(gl_FragCoord.x, uViewport.y - gl_FragCoord.y);
    vec2 pc = surface / uScale + uPan;

    // Nearest monitor edge. Negative inside a monitor, so the union of the
    // screens is exactly where this is below zero — which is also exactly
    // where touches are accepted.
    float best = 1.0e9;
    for (int i = 0; i < MAX_MONITORS; ++i) {
        if (i >= uMonitorCount) {
            break;
        }
        best = min(best, sdRect(pc, uMonitors[i]));
    }
    float sd = best * uScale;

    vec3 col = vec3(0.043, 0.047, 0.059);

    // Area that actually maps to a screen reads brighter.
    col = mix(col, vec3(0.078, 0.086, 0.105), 1.0 - smoothstep(-1.0, 1.0, sd));

    float minor = max(lineAlpha(pc.x, uMinorStep, 0.5),
                      lineAlpha(pc.y, uMinorStep, 0.5));
    float major = max(lineAlpha(pc.x, uMinorStep * 5.0, 0.85),
                      lineAlpha(pc.y, uMinorStep * 5.0, 0.85));

    // Outside the screens the grid is dimmed: a visible reminder that a touch
    // there goes nowhere.
    float live = 1.0 - smoothstep(-1.0, 1.0, sd);
    float grid_gain = mix(0.35, 1.0, live);

    col = mix(col, uAccent * 0.55, minor * 0.45 * grid_gain);
    col = mix(col, uAccent, major * 0.7 * grid_gain);

    // Screen outlines last so they stay readable over the grid.
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
    u_monitor_count_ = glGetUniformLocation(program_, "uMonitorCount");
    u_monitors_ = glGetUniformLocation(program_, "uMonitors");
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
                        std::span<const core::Recti> monitors, bool injection_enabled,
                        bool linked) {
    if (program_ == 0 || monitors.empty()) {
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

    const int count =
        static_cast<int>(std::min<std::size_t>(monitors.size(), kMaxDrawnMonitors));
    float packed[kMaxDrawnMonitors * 4] = {};
    for (int i = 0; i < count; ++i) {
        packed[i * 4 + 0] = static_cast<float>(monitors[static_cast<std::size_t>(i)].x);
        packed[i * 4 + 1] = static_cast<float>(monitors[static_cast<std::size_t>(i)].y);
        packed[i * 4 + 2] = static_cast<float>(monitors[static_cast<std::size_t>(i)].w);
        packed[i * 4 + 3] = static_cast<float>(monitors[static_cast<std::size_t>(i)].h);
    }
    glUniform1i(u_monitor_count_, count);
    glUniform4fv(u_monitors_, count, packed);

    glUniform3fv(u_accent_, 1, accent);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

} // namespace digitiz::guest
