#include "ui/SideMenu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace digitiz::guest {

namespace {

constexpr double kSlideSeconds = 0.16;

float ease_out(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t);
}

const Color kAccent{0.30f, 0.62f, 0.46f, 1.0f};
const Color kIdleGlyph{0.62f, 0.66f, 0.74f, 1.0f};

// Ranges for the decimation sliders. Both start at zero, which is "send
// everything" — the behaviour before this existed.
constexpr int kMaxIntervalMs = 40;
constexpr float kMaxDistanceDp = 16.0f;

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

bool SideMenu::take_auto_launch_change() noexcept {
    const bool changed = auto_launch_changed_;
    auto_launch_changed_ = false;
    return changed;
}

bool SideMenu::take_rotate_request() noexcept {
    const bool asked = rotate_requested_;
    rotate_requested_ = false;
    return asked;
}

void SideMenu::set_throttle(int interval_ms, float distance_dp) noexcept {
    min_interval_ms_ = std::clamp(interval_ms, 0, kMaxIntervalMs);
    min_distance_dp_ = std::clamp(distance_dp, 0.0f, kMaxDistanceDp);
}

bool SideMenu::take_throttle_change() noexcept {
    const bool changed = throttle_changed_;
    throttle_changed_ = false;
    return changed;
}

Rect SideMenu::throttle_row(int index) const {
    const Rect above = auto_launch_row();
    // Heights are already in pixels here; multiplying the whole expression by
    // density again is the easy mistake.
    const float h = 64.0f * density_;
    const float gap = 8.0f * density_;
    const float top = above.y + above.h + 12.0f * density_ + static_cast<float>(index) * (h + gap);
    return Rect{above.x, top, above.w, h};
}

Rect SideMenu::throttle_track(int index) const {
    const Rect row = throttle_row(index);
    const float pad = 16.0f * density_;
    return Rect{row.x + pad, row.y + row.h - 22.0f * density_, row.w - pad * 2.0f,
                6.0f * density_};
}

float SideMenu::throttle_fraction(int index) const {
    if (index == 0) {
        return static_cast<float>(min_interval_ms_) / static_cast<float>(kMaxIntervalMs);
    }
    return min_distance_dp_ / kMaxDistanceDp;
}

void SideMenu::set_throttle_from_x(int index, float x) {
    const Rect track = throttle_track(index);
    const float t = std::clamp((x - track.x) / track.w, 0.0f, 1.0f);

    if (index == 0) {
        const int ms = static_cast<int>(std::lround(t * kMaxIntervalMs));
        if (ms != min_interval_ms_) {
            min_interval_ms_ = ms;
            throttle_changed_ = true;
        }
        return;
    }

    // Half-dp steps: finer than that is not something a thumb can aim at.
    const float dp = std::round(t * kMaxDistanceDp * 2.0f) * 0.5f;
    if (dp != min_distance_dp_) {
        min_distance_dp_ = dp;
        throttle_changed_ = true;
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

// Beside the title rather than in a row of its own. Four rows already reach
// within a row's height of the bottom on a 720px screen, and this is a
// one-shot action with no state to display, which is what a header control is
// for.
Rect SideMenu::rotate_button() const {
    const Rect panel = panel_rect();
    const float pad = 16.0f * density_;
    const float w = 104.0f * density_;
    const float h = 34.0f * density_;
    return Rect{panel.x + panel.w - pad - w, 12.0f * density_, w, h};
}

Rect SideMenu::mode_row() const {
    const Rect panel = panel_rect();
    const float pad = 16.0f * density_;
    return Rect{panel.x + pad, 62.0f * density_, panel.w - pad * 2.0f, 56.0f * density_};
}

Rect SideMenu::mode_value_pill() const {
    const Rect row = mode_row();
    const float w = 118.0f * density_;
    const float h = 36.0f * density_;
    return Rect{row.x + row.w - w - 10.0f * density_, row.y + (row.h - h) * 0.5f, w, h};
}

Rect SideMenu::auto_launch_row() const {
    const Rect above = mode_row();
    return Rect{above.x, above.y + above.h + 12.0f * density_, above.w, above.h};
}

Rect SideMenu::auto_launch_switch() const {
    const Rect row = auto_launch_row();
    const float w = 52.0f * density_;
    const float h = 30.0f * density_;
    return Rect{row.x + row.w - w - 12.0f * density_, row.y + (row.h - h) * 0.5f, w, h};
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
        // The whole row is the target, not just the control on its right: a
        // 36dp pill is a small thing to hit with a thumb.
        if (progress_ > 0.9f) {
            if (rotate_button().contains(p)) {
                rotate_requested_ = true;
            } else if (mode_row().contains(p)) {
                // Few enough modes that cycling beats a picker.
                mode_ = next_mode(mode_);
                mode_changed_ = true;
            } else if (auto_launch_row().contains(p)) {
                auto_launch_ = !auto_launch_;
                auto_launch_changed_ = true;
            } else {
                for (int i = 0; i < 2; ++i) {
                    if (!throttle_row(i).contains(p)) {
                        continue;
                    }
                    // Jump to where the finger landed, then track it.
                    dragging_slider_ = i;
                    set_throttle_from_x(i, static_cast<float>(p.x));
                    break;
                }
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

// A dot for Draw — one point, which is what a stroke puts on the PC. A knob on
// a track for Slide, which is what the finger does there. A cross for Pan,
// read as "move in any direction".
void SideMenu::draw_mode_glyph(UiRenderer& ui, float cx, float cy, Color color,
                               float scale) const {
    const float unit = density_ * scale;
    switch (mode_) {
    case InputMode::Draw: {
        const float d = 12.0f * unit;
        ui.rounded_rect(Rect{cx - d * 0.5f, cy - d * 0.5f, d, d}, d * 0.5f, color);
        break;
    }
    case InputMode::Slide: {
        const float len = 20.0f * unit;
        const float track = 3.0f * unit;
        const float knob = 10.0f * unit;
        ui.rounded_rect(Rect{cx - len * 0.5f, cy - track * 0.5f, len, track}, track * 0.5f,
                        Color{color.r, color.g, color.b, color.a * 0.55f});
        ui.rounded_rect(Rect{cx + len * 0.5f - knob, cy - knob * 0.5f, knob, knob}, knob * 0.5f,
                        color);
        break;
    }
    case InputMode::Pan: {
        const float len = 18.0f * unit;
        const float bar = 3.0f * unit;
        ui.rounded_rect(Rect{cx - len * 0.5f, cy - bar * 0.5f, len, bar}, bar * 0.5f, color);
        ui.rounded_rect(Rect{cx - bar * 0.5f, cy - len * 0.5f, bar, len}, bar * 0.5f, color);
        break;
    }
    }
}

void SideMenu::draw_throttle_row(UiRenderer& ui, int index, float alpha) const {
    const Rect row = throttle_row(index);
    ui.rounded_rect(row, 12.0f * density_, Color{0.16f, 0.17f, 0.21f, 0.9f * alpha});

    const Rect track = throttle_track(index);
    const float t = throttle_fraction(index);
    const bool off = t <= 0.0f;

    ui.rounded_rect(track, track.h * 0.5f, Color{0.26f, 0.27f, 0.32f, alpha});
    if (!off) {
        ui.rounded_rect(Rect{track.x, track.y, track.w * t, track.h}, track.h * 0.5f,
                        Color{kAccent.r, kAccent.g, kAccent.b, alpha});
    }

    const float knob = 20.0f * density_;
    ui.rounded_rect(Rect{track.x + track.w * t - knob * 0.5f, track.y + track.h * 0.5f - knob * 0.5f,
                         knob, knob},
                    knob * 0.5f,
                    off ? Color{0.62f, 0.66f, 0.74f, alpha} : Color{0.96f, 0.97f, 1.0f, alpha});
}

void SideMenu::draw_mode_row(UiRenderer& ui, float alpha) const {
    const Rect row = mode_row();
    ui.rounded_rect(row, 12.0f * density_, Color{0.16f, 0.17f, 0.21f, 0.9f * alpha});

    // The pill carries the current value. Green whenever the finger reaches
    // the PC at all, neutral when it only moves the view — the same reading as
    // the grid tint.
    const Rect pill = mode_value_pill();
    const bool reaches_pc = mode_ != InputMode::Pan;
    ui.rounded_rect(pill, pill.h * 0.5f,
                    reaches_pc ? Color{kAccent.r, kAccent.g, kAccent.b, 0.95f * alpha}
                               : Color{0.26f, 0.27f, 0.32f, 0.95f * alpha});

    draw_mode_glyph(ui, pill.x + 20.0f * density_, pill.y + pill.h * 0.5f,
                    reaches_pc ? Color{1.0f, 1.0f, 1.0f, alpha}
                               : Color{kIdleGlyph.r, kIdleGlyph.g, kIdleGlyph.b, alpha});
}

// "The PC may open this app by itself": a phone with something arriving at it,
// plus a switch. The label beside it does the explaining; the pictogram is
// there to make the row scannable.
void SideMenu::draw_auto_launch_row(UiRenderer& ui, float alpha) const {
    const Rect row = auto_launch_row();
    ui.rounded_rect(row, 12.0f * density_, Color{0.16f, 0.17f, 0.21f, 0.9f * alpha});

    // Left to right: something travels from the PC and lands in the phone.
    // Order matters — phone first would read as the app sending, which is
    // backwards.
    const float bar_h = 3.0f * density_;
    const float bar_w = 16.0f * density_;
    const float head = 9.0f * density_;
    const Color arrow = auto_launch_ ? Color{0.55f, 0.90f, 0.72f, alpha}
                                     : Color{0.42f, 0.45f, 0.52f, alpha};

    const float bar_x = row.x + 12.0f * density_;
    ui.rounded_rect(Rect{bar_x, row.y + (row.h - bar_h) * 0.5f, bar_w, bar_h}, bar_h * 0.5f,
                    arrow);
    ui.rounded_rect(Rect{bar_x + bar_w - head * 0.5f, row.y + (row.h - head) * 0.5f, head, head},
                    head * 0.5f, arrow);

    // Phone outline, receiving it.
    const float ph = 30.0f * density_;
    const float pw = 18.0f * density_;
    const Rect phone{bar_x + bar_w + head * 0.5f + 5.0f * density_, row.y + (row.h - ph) * 0.5f,
                     pw, ph};
    ui.rounded_rect_outline(phone, 4.0f * density_, 1.5f * density_,
                            auto_launch_ ? Color{0.80f, 0.86f, 0.92f, alpha}
                                         : Color{0.50f, 0.53f, 0.60f, alpha});

    // Switch.
    const Rect sw = auto_launch_switch();
    ui.rounded_rect(sw, sw.h * 0.5f,
                    auto_launch_ ? Color{kAccent.r, kAccent.g, kAccent.b, 0.95f * alpha}
                                 : Color{0.24f, 0.25f, 0.30f, 0.95f * alpha});

    const float knob = sw.h - 6.0f * density_;
    const float knob_x = auto_launch_ ? sw.x + sw.w - knob - 3.0f * density_
                                      : sw.x + 3.0f * density_;
    ui.rounded_rect(Rect{knob_x, sw.y + 3.0f * density_, knob, knob}, knob * 0.5f,
                    Color{0.96f, 0.97f, 1.0f, alpha});
}

// No state shown, because there is none to show: the drawer turns with
// everything else, so both orientations look identical from inside it. Only
// the phone on the desk can say which way round it is.
void SideMenu::draw_rotate_button(UiRenderer& ui, float alpha) const {
    const Rect button = rotate_button();
    ui.rounded_rect(button, button.h * 0.5f, Color{0.22f, 0.24f, 0.29f, 0.95f * alpha});
}

void SideMenu::drag(core::Vec2 p) {
    if (dragging_slider_ >= 0) {
        // Tracked by x only: the finger wandering off the track vertically
        // should not abandon the drag.
        set_throttle_from_x(dragging_slider_, static_cast<float>(p.x));
    }
}

void SideMenu::release(core::Vec2 p) {
    if (dragging_slider_ >= 0) {
        set_throttle_from_x(dragging_slider_, static_cast<float>(p.x));
        dragging_slider_ = -1;
    }
}

void SideMenu::load_labels(TextRenderer& text) {
    if (labels_loaded_) {
        return;
    }
    labels_.title = text.localized("menu_title");
    labels_.input_mode = text.localized("menu_input_mode");
    labels_.draw = text.localized("menu_mode_draw");
    labels_.slide = text.localized("menu_mode_slide");
    labels_.pan = text.localized("menu_mode_pan");
    labels_.auto_launch = text.localized("menu_auto_launch");
    labels_.rotate = text.localized("menu_rotate");
    labels_.throttle_time = text.localized("menu_throttle_time");
    labels_.throttle_distance = text.localized("menu_throttle_distance");
    labels_.throttle_off = text.localized("menu_throttle_off");
    labels_loaded_ = true;
}

void SideMenu::draw_labels(TextRenderer& text) const {
    if (progress_ <= 0.01f || !labels_loaded_) {
        return;
    }

    const Rect panel = panel_rect();
    const float pad = 16.0f * density_;
    const float a = progress_;

    text.draw(labels_.title, panel.x + pad, 20.0f * density_, 17.0f * density_,
              Color{0.92f, 0.94f, 0.98f, a}, TextAlign::Left, true);

    const Rect rotate = rotate_button();
    text.draw(labels_.rotate, rotate.x + rotate.w * 0.5f, rotate.y + rotate.h * 0.5f - 9.0f * density_,
              13.0f * density_, Color{0.86f, 0.89f, 0.95f, a}, TextAlign::Center);

    // Two rows of the same shape: setting name left, current value right.
    const float text_size = 14.0f * density_;
    const float half_line = 10.0f * density_;

    const Rect mode = mode_row();
    text.draw(labels_.input_mode, mode.x + 16.0f * density_, mode.y + mode.h * 0.5f - half_line,
              text_size, Color{0.80f, 0.84f, 0.90f, a});

    const Rect pill = mode_value_pill();
    const std::string* value = &labels_.draw;
    if (mode_ == InputMode::Slide) {
        value = &labels_.slide;
    } else if (mode_ == InputMode::Pan) {
        value = &labels_.pan;
    }
    text.draw(*value, pill.x + 36.0f * density_, pill.y + pill.h * 0.5f - half_line, text_size,
              mode_ != InputMode::Pan ? Color{1.0f, 1.0f, 1.0f, a}
                                      : Color{0.80f, 0.84f, 0.90f, a});

    const Rect row = auto_launch_row();
    text.draw(labels_.auto_launch, row.x + 62.0f * density_, row.y + row.h * 0.5f - half_line,
              text_size,
              auto_launch_ ? Color{0.90f, 0.93f, 0.97f, a} : Color{0.60f, 0.63f, 0.70f, a});

    // Decimation sliders: name on the left, value on the right, track below.
    for (int i = 0; i < 2; ++i) {
        const Rect slider_row = throttle_row(i);
        const float pad = 16.0f * density_;
        const float label_y = slider_row.y + 12.0f * density_;
        const bool off = throttle_fraction(i) <= 0.0f;

        text.draw(i == 0 ? labels_.throttle_time : labels_.throttle_distance, slider_row.x + pad,
                  label_y, text_size, Color{0.80f, 0.84f, 0.90f, a});

        char value[32];
        if (off) {
            std::snprintf(value, sizeof(value), "%s", labels_.throttle_off.c_str());
        } else if (i == 0) {
            std::snprintf(value, sizeof(value), "%d ms", min_interval_ms_);
        } else {
            std::snprintf(value, sizeof(value), "%.1f dp", static_cast<double>(min_distance_dp_));
        }
        text.draw(value, slider_row.x + slider_row.w - pad, label_y, text_size,
                  off ? Color{0.55f, 0.58f, 0.66f, a} : Color{0.55f, 0.92f, 0.75f, a},
                  TextAlign::Right);
    }
}

void SideMenu::draw(UiRenderer& ui) const {
    if (progress_ > 0.01f) {
        const Rect panel = panel_rect();
        ui.rounded_rect(panel, 18.0f * density_,
                        Color{0.11f, 0.12f, 0.145f, 0.96f * progress_ + 0.04f});

        draw_rotate_button(ui, progress_);
        draw_mode_row(ui, progress_);
        draw_auto_launch_row(ui, progress_);
        draw_throttle_row(ui, 0, progress_);
        draw_throttle_row(ui, 1, progress_);
    }

    const Rect handle = handle_rect();
    ui.rounded_rect(handle, handle.w * 0.45f, Color{0.20f, 0.22f, 0.27f, 0.95f});

    // The handle carries the active mode, so the drawer does not have to be
    // open to know what a finger will do. Same glyph as the pill, shrunk to
    // fit a 22dp tab.
    draw_mode_glyph(ui, handle.x + handle.w * 0.5f, handle.y + handle.h * 0.5f,
                    mode_ == InputMode::Pan ? Color{0.85f, 0.88f, 0.95f, 0.95f}
                                            : Color{0.55f, 0.92f, 0.75f, 0.95f},
                    0.62f);
}

} // namespace digitiz::guest
