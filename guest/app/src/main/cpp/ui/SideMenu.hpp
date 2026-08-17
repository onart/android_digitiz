#pragma once

// Edge handle plus the drawer it toggles.
//
// The drawer holds the input mode switch. The handle shows which mode is
// active, so the answer to "will this finger draw on the PC?" is on screen
// without opening anything.

#include <digitiz/core/geometry.hpp>

#include "input/TouchRouter.hpp"
#include "render/UiRenderer.hpp"

namespace digitiz::guest {

class SideMenu {
public:
    void layout(int surface_w, int surface_h, float density);
    void advance(double dt_seconds);

    // True if the point belongs to the menu, in which case it must not reach
    // the digitizer. Also drives the toggle and the mode switch.
    bool hit_test(core::Vec2 p);

    void draw(UiRenderer& ui) const;

    bool open() const noexcept { return target_ > 0.5f; }

    InputMode mode() const noexcept { return mode_; }
    // True once, when hit_test changed the mode; the caller pushes it onward.
    bool take_mode_change() noexcept;

private:
    Rect handle_rect() const;
    Rect panel_rect() const;
    Rect mode_cell(int index) const; // 0 = draw, 1 = pan

    void draw_draw_glyph(UiRenderer& ui, Rect cell, float alpha) const;
    void draw_pan_glyph(UiRenderer& ui, Rect cell, float alpha) const;

    int surface_w_ = 0;
    int surface_h_ = 0;
    float density_ = 1.0f;

    float panel_w_ = 0.0f;
    float handle_w_ = 0.0f;
    float handle_h_ = 0.0f;

    // 0 closed, 1 open; `progress_` chases `target_`.
    float target_ = 0.0f;
    float progress_ = 0.0f;

    InputMode mode_ = InputMode::Draw;
    bool mode_changed_ = false;
};

} // namespace digitiz::guest
