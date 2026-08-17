#pragma once

// Draws the PC coordinate grid.
//
// One fullscreen triangle; the fragment shader inverts the view transform to
// get PC pixels and evaluates the grid analytically. That gives free
// anti-aliasing at any zoom and makes the level-of-detail switch a single
// log-scale step, with no geometry to rebuild when the user pinches.

#include <span>

#include <GLES3/gl3.h>

#include <digitiz/core/geometry.hpp>

namespace digitiz::guest {

// The shader draws each monitor separately rather than one bounding box,
// because the bounding box is not where input is accepted: two monitors of
// different heights leave a corner inside the box that belongs to neither.
// Showing the real shape is what makes "why did my touch do nothing" obvious.
inline constexpr int kMaxDrawnMonitors = 8;

class GridRenderer {
public:
    bool init();
    void release();

    void draw(const core::ViewTransform& view, int surface_w, int surface_h,
              std::span<const core::Recti> monitors, bool injection_enabled, bool linked);

private:
    GLuint program_ = 0;
    GLuint vao_ = 0;

    GLint u_viewport_ = -1;
    GLint u_scale_ = -1;
    GLint u_pan_ = -1;
    GLint u_minor_step_ = -1;
    GLint u_monitor_count_ = -1;
    GLint u_monitors_ = -1;
    GLint u_accent_ = -1;
};

} // namespace digitiz::guest
