#pragma once

// Rounded rectangles, which is the whole widget vocabulary milestone 1 needs.
//
// Each shape draws a quad covering only its own bounds rather than a fullscreen
// pass, so the fragment cost stays proportional to the widget, not the screen.

#include <GLES3/gl3.h>

#include <digitiz/core/geometry.hpp>

namespace digitiz::guest {

struct Rect {
    float x = 0.0f;
    float y = 0.0f; // top-left origin, surface pixels
    float w = 0.0f;
    float h = 0.0f;

    bool contains(core::Vec2 p) const noexcept {
        return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
    }
};

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

class UiRenderer {
public:
    bool init();
    void release();

    void begin(int surface_w, int surface_h);
    void end();

    void rounded_rect(Rect rect, float radius, Color fill);

private:
    GLuint program_ = 0;
    GLuint vao_ = 0;

    GLint u_viewport_ = -1;
    GLint u_rect_ = -1;
    GLint u_radius_ = -1;
    GLint u_color_ = -1;

    int surface_w_ = 0;
    int surface_h_ = 0;
};

} // namespace digitiz::guest
