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
    // the digitizer. Also drives the toggle and the mode switch.
    bool hit_test(core::Vec2 p);

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

private:
    Rect handle_rect() const;
    Rect panel_rect() const;
    Rect mode_cell(int index) const; // 0 = draw, 1 = pan
    Rect auto_launch_row() const;
    Rect auto_launch_switch() const;

    void draw_draw_glyph(UiRenderer& ui, Rect cell, float alpha) const;
    void draw_pan_glyph(UiRenderer& ui, Rect cell, float alpha) const;
    void draw_auto_launch_row(UiRenderer& ui, float alpha) const;

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

    struct Labels {
        std::string title;
        std::string input_mode;
        std::string draw;
        std::string pan;
        std::string auto_launch;
    };
    Labels labels_;
    bool labels_loaded_ = false;
};

} // namespace digitiz::guest
