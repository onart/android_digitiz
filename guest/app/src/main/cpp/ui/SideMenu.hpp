#pragma once

// Edge handle plus the drawer it toggles.
//
// Milestone 1 deliberately leaves the drawer empty: the point is the toggle
// affordance and the guarantee that touching it never leaks through as a
// digitizer stroke. Milestone 2 fills it with the custom button list.

#include <digitiz/core/geometry.hpp>

#include "render/UiRenderer.hpp"

namespace digitiz::guest {

class SideMenu {
public:
    void layout(int surface_w, int surface_h, float density);
    void advance(double dt_seconds);

    // True if the point belongs to the menu, in which case it must not reach
    // the digitizer. Also drives the toggle.
    bool hit_test(core::Vec2 p);

    void draw(UiRenderer& ui) const;

    bool open() const noexcept { return target_ > 0.5f; }

private:
    Rect handle_rect() const;
    Rect panel_rect() const;

    int surface_w_ = 0;
    int surface_h_ = 0;
    float density_ = 1.0f;

    float panel_w_ = 0.0f;
    float handle_w_ = 0.0f;
    float handle_h_ = 0.0f;

    // 0 closed, 1 open; `progress_` chases `target_`.
    float target_ = 0.0f;
    float progress_ = 0.0f;
};

} // namespace digitiz::guest
