#include "ui/SideMenu.hpp"

#include <algorithm>
#include <cmath>

namespace digitiz::guest {

namespace {

constexpr double kSlideSeconds = 0.16;

float ease_out(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t);
}

} // namespace

void SideMenu::layout(int surface_w, int surface_h, float density) {
    surface_w_ = surface_w;
    surface_h_ = surface_h;
    density_ = density > 0.0f ? density : 1.0f;

    // Comfortable thumb target: ~22dp wide, ~110dp tall.
    handle_w_ = 22.0f * density_;
    handle_h_ = 110.0f * density_;

    const float max_panel = 300.0f * density_;
    panel_w_ = std::min(static_cast<float>(surface_w) * 0.62f, max_panel);
}

void SideMenu::advance(double dt_seconds) {
    const float step = static_cast<float>(dt_seconds / kSlideSeconds);
    if (progress_ < target_) {
        progress_ = std::min(target_, progress_ + step);
    } else if (progress_ > target_) {
        progress_ = std::max(target_, progress_ - step);
    }
}

Rect SideMenu::handle_rect() const {
    const float slide = ease_out(progress_) * panel_w_;
    return Rect{
        static_cast<float>(surface_w_) - handle_w_ - slide,
        (static_cast<float>(surface_h_) - handle_h_) * 0.5f,
        handle_w_,
        handle_h_,
    };
}

Rect SideMenu::panel_rect() const {
    const float slide = ease_out(progress_) * panel_w_;
    return Rect{
        static_cast<float>(surface_w_) - slide,
        0.0f,
        panel_w_,
        static_cast<float>(surface_h_),
    };
}

bool SideMenu::hit_test(core::Vec2 p) {
    // Generous touch slop around the handle: it is a thin tab at the screen
    // edge, and missing it by two pixels would draw a stray dot on the PC.
    Rect handle = handle_rect();
    const float slop = 12.0f * density_;
    handle.x -= slop;
    handle.y -= slop;
    handle.w += slop * 2.0f;
    handle.h += slop * 2.0f;

    if (handle.contains(p)) {
        target_ = target_ > 0.5f ? 0.0f : 1.0f;
        return true;
    }

    if (progress_ > 0.01f && panel_rect().contains(p)) {
        return true; // swallow taps inside the drawer
    }

    if (target_ > 0.5f) {
        target_ = 0.0f; // tapping away closes it, and that tap is consumed
        return true;
    }

    return false;
}

void SideMenu::draw(UiRenderer& ui) const {
    if (progress_ > 0.01f) {
        const Rect panel = panel_rect();
        ui.rounded_rect(panel, 18.0f * density_,
                        Color{0.11f, 0.12f, 0.145f, 0.96f * progress_ + 0.04f});

        // Header strip, so an empty drawer still reads as a panel.
        ui.rounded_rect(Rect{panel.x + 14.0f * density_, 18.0f * density_,
                             panel.w - 28.0f * density_, 4.0f * density_},
                        2.0f * density_, Color{0.30f, 0.34f, 0.42f, progress_});
    }

    const Rect handle = handle_rect();
    ui.rounded_rect(handle, handle.w * 0.45f, Color{0.20f, 0.22f, 0.27f, 0.95f});

    // Grip line down the middle of the tab.
    const float grip_w = 2.0f * density_;
    ui.rounded_rect(Rect{handle.x + (handle.w - grip_w) * 0.5f, handle.y + handle.h * 0.28f,
                         grip_w, handle.h * 0.44f},
                    grip_w * 0.5f, Color{0.55f, 0.60f, 0.70f, 0.9f});
}

} // namespace digitiz::guest
