#pragma once

// Edge handle plus the drawer it toggles.
//
// The drawer holds the input mode switch. The handle shows which mode is
// active, so the answer to "will this finger draw on the PC?" is on screen
// without opening anything.

#include <digitiz/core/geometry.hpp>

#include <string>

#include "input/TouchRouter.hpp"
#include "render/UiRenderer.hpp"
#include "text/TextRenderer.hpp"

namespace digitiz::guest {

class SideMenu {
public:
    void layout(int surface_w, int surface_h, float density);
    void advance(double dt_seconds);

    // True if the point belongs to the menu, in which case it must not reach
    // the digitizer, and the menu captures the finger until it lifts.
    bool hit_test(core::Vec2 p);
    void drag(core::Vec2 p);
    void release(core::Vec2 p);
    // Abandons a press without acting on it.
    void cancel_press() noexcept { dragging_slider_ = -1; }

    // Shapes and text use different programs, so they are drawn in two passes
    // rather than interleaved.
    void draw(UiRenderer& ui) const;
    void draw_labels(TextRenderer& text) const;

    // Pulls label text from strings.xml once the text renderer is up.
    void load_labels(TextRenderer& text);

    bool open() const noexcept { return target_ > 0.5f; }

    InputMode mode() const noexcept { return mode_; }
    // True once, when hit_test changed the mode; the caller pushes it onward.
    bool take_mode_change() noexcept;

    // Whether the PC may start this app by itself. Seeded from persisted
    // settings; take_auto_launch_change() reports a user flip so the caller
    // can write it back.
    void set_auto_launch(bool on) noexcept { auto_launch_ = on; }
    bool auto_launch() const noexcept { return auto_launch_; }
    bool take_auto_launch_change() noexcept;

    // True once, when the user asked for the display to be turned round. The
    // menu holds no state for it: the orientation belongs to the activity, and
    // asking it back would only be a second copy that can disagree.
    bool take_rotate_request() noexcept;

    // Which way the custom button strip runs. Seeded from persisted settings,
    // flipped by the header button beside the rotate one.
    void set_strip_vertical(bool on) noexcept { strip_vertical_ = on; }
    bool strip_vertical() const noexcept { return strip_vertical_; }
    bool take_strip_change() noexcept;

    // Pointer decimation while drawing, in milliseconds and dp.
    void set_throttle(int interval_ms, float distance_dp) noexcept;
    int min_interval_ms() const noexcept { return min_interval_ms_; }
    float min_distance_dp() const noexcept { return min_distance_dp_; }
    bool take_throttle_change() noexcept;

    // How many buttons the strip shows at once. `max` comes from the strip,
    // which is the only thing that knows what the screen can hold in the
    // direction it is currently running.
    void set_strip_slots(int slots, int max) noexcept;
    int strip_slots() const noexcept { return strip_slots_; }
    bool take_strip_slots_change() noexcept;

private:
    Rect handle_rect() const;
    Rect panel_rect() const;
    // Two settings rows of the same shape: a name on the left, the current
    // value on the right, tap anywhere on the row to change it.
    Rect mode_row() const;
    Rect mode_value_pill() const;
    Rect auto_launch_row() const;
    Rect auto_launch_switch() const;
    Rect rotate_button() const;
    Rect strip_button() const;

    // 0 = min interval, 1 = min distance, 2 = strip length. Three sliders of
    // the same shape, so they share their geometry and their drag handling.
    static constexpr int kSliderCount = 3;
    Rect slider_row(int index) const;
    Rect slider_track(int index) const;
    float slider_fraction(int index) const;
    void set_slider_from_x(int index, float x);
    void draw_slider_row(UiRenderer& ui, int index, float alpha) const;

    void draw_mode_row(UiRenderer& ui, float alpha) const;
    void draw_mode_glyph(UiRenderer& ui, float cx, float cy, Color color, float scale = 1.0f) const;
    void draw_auto_launch_row(UiRenderer& ui, float alpha) const;
    void draw_rotate_button(UiRenderer& ui, float alpha) const;
    void draw_strip_button(UiRenderer& ui, float alpha) const;

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

    bool auto_launch_ = true;
    bool auto_launch_changed_ = false;

    bool rotate_requested_ = false;

    bool strip_vertical_ = false;
    bool strip_changed_ = false;

    int min_interval_ms_ = 0;
    float min_distance_dp_ = 0.0f;
    bool throttle_changed_ = false;

    int strip_slots_ = 1;
    int strip_slots_max_ = 1;
    bool strip_slots_changed_ = false;
    // Which slider owns the finger, -1 for none.
    int dragging_slider_ = -1;

    struct Labels {
        std::string title;
        std::string input_mode;
        std::string draw;
        std::string slide;
        std::string pan;
        std::string auto_launch;
        std::string rotate;

        std::string throttle_time;
        std::string throttle_distance;
        std::string throttle_off;
        std::string strip_length;
    };
    Labels labels_;
    bool labels_loaded_ = false;
};

} // namespace digitiz::guest
