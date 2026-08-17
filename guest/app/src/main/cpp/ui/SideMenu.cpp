#include "ui/SideMenu.hpp"

#include <algorithm>
#include <cmath>

namespace digitiz::guest {

namespace {

constexpr double kSlideSeconds = 0.16;

float ease_out(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t);
}

const Color kAccent{0.30f, 0.62f, 0.46f, 1.0f};
const Color kIdleGlyph{0.62f, 0.66f, 0.74f, 1.0f};

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

bool SideMenu::take_mode_change() noexcept {
    const bool changed = mode_changed_;
    mode_changed_ = false;
    return changed;
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

Rect SideMenu::mode_cell(int index) const {
    const Rect panel = panel_rect();
    const float pad = 16.0f * density_;
    const float gap = 10.0f * density_;
    const float top = 44.0f * density_;
    const float height = 68.0f * density_;
    const float width = (panel.w - pad * 2.0f - gap) * 0.5f;

    return Rect{panel.x + pad + static_cast<float>(index) * (width + gap), top, width, height};
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
        // Only act on the switch once the drawer has essentially arrived, so a
        // tap during the slide does not land on a cell that has moved.
        if (progress_ > 0.9f) {
            for (int i = 0; i < 2; ++i) {
                if (!mode_cell(i).contains(p)) {
                    continue;
                }
                const InputMode picked = i == 0 ? InputMode::Draw : InputMode::Pan;
                if (picked != mode_) {
                    mode_ = picked;
                    mode_changed_ = true;
                }
                break;
            }
        }
        return true; // swallow every tap inside the drawer
    }

    if (target_ > 0.5f) {
        target_ = 0.0f; // tapping away closes it, and that tap is consumed
        return true;
    }

    return false;
}

// A filled dot: one point, which is what a stroke puts on the PC.
void SideMenu::draw_draw_glyph(UiRenderer& ui, Rect cell, float alpha) const {
    const float d = 14.0f * density_;
    const Color c = mode_ == InputMode::Draw ? Color{1.0f, 1.0f, 1.0f, alpha}
                                             : Color{kIdleGlyph.r, kIdleGlyph.g, kIdleGlyph.b, alpha};
    ui.rounded_rect(Rect{cell.x + (cell.w - d) * 0.5f, cell.y + (cell.h - d) * 0.5f, d, d}, d * 0.5f,
                    c);
}

// A cross, read as "move in any direction".
void SideMenu::draw_pan_glyph(UiRenderer& ui, Rect cell, float alpha) const {
    const float len = 22.0f * density_;
    const float bar = 4.0f * density_;
    const Color c = mode_ == InputMode::Pan ? Color{1.0f, 1.0f, 1.0f, alpha}
                                            : Color{kIdleGlyph.r, kIdleGlyph.g, kIdleGlyph.b, alpha};

    const float cx = cell.x + cell.w * 0.5f;
    const float cy = cell.y + cell.h * 0.5f;
    ui.rounded_rect(Rect{cx - len * 0.5f, cy - bar * 0.5f, len, bar}, bar * 0.5f, c);
    ui.rounded_rect(Rect{cx - bar * 0.5f, cy - len * 0.5f, bar, len}, bar * 0.5f, c);
}

void SideMenu::draw(UiRenderer& ui) const {
    if (progress_ > 0.01f) {
        const Rect panel = panel_rect();
        ui.rounded_rect(panel, 18.0f * density_,
                        Color{0.11f, 0.12f, 0.145f, 0.96f * progress_ + 0.04f});

        // Header strip.
        ui.rounded_rect(Rect{panel.x + 16.0f * density_, 18.0f * density_,
                             panel.w - 32.0f * density_, 4.0f * density_},
                        2.0f * density_, Color{0.30f, 0.34f, 0.42f, progress_});

        for (int i = 0; i < 2; ++i) {
            const Rect cell = mode_cell(i);
            const bool active = (i == 0) == (mode_ == InputMode::Draw);

            ui.rounded_rect(cell, 12.0f * density_,
                            active ? Color{kAccent.r, kAccent.g, kAccent.b, 0.85f * progress_}
                                   : Color{0.18f, 0.19f, 0.23f, 0.9f * progress_});
            if (active) {
                ui.rounded_rect_outline(cell, 12.0f * density_, 1.5f * density_,
                                        Color{0.55f, 0.90f, 0.72f, progress_});
            }
        }

        draw_draw_glyph(ui, mode_cell(0), progress_);
        draw_pan_glyph(ui, mode_cell(1), progress_);
    }

    const Rect handle = handle_rect();
    ui.rounded_rect(handle, handle.w * 0.45f, Color{0.20f, 0.22f, 0.27f, 0.95f});

    // The handle carries the active mode, so the drawer does not have to be
    // open to know whether a finger will draw.
    const float cx = handle.x + handle.w * 0.5f;
    const float cy = handle.y + handle.h * 0.5f;
    if (mode_ == InputMode::Draw) {
        const float d = 8.0f * density_;
        ui.rounded_rect(Rect{cx - d * 0.5f, cy - d * 0.5f, d, d}, d * 0.5f,
                        Color{kAccent.r + 0.25f, 0.92f, 0.75f, 0.95f});
    } else {
        const float len = 14.0f * density_;
        const float bar = 3.0f * density_;
        const Color c{0.85f, 0.88f, 0.95f, 0.95f};
        ui.rounded_rect(Rect{cx - bar * 0.5f, cy - len * 0.5f, bar, len}, bar * 0.5f, c);
        ui.rounded_rect(Rect{cx - len * 0.5f, cy - bar * 0.5f, len, bar}, bar * 0.5f, c);
    }
}

} // namespace digitiz::guest
