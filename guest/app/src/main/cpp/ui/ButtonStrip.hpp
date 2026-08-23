#pragma once

// The row (or column) of custom buttons.
//
// Lives on one screen edge and collapses to a tab, because every pixel it
// occupies is a pixel that cannot be drawn on. When there are more buttons
// than fit, the list scrolls along its own direction -- by dragging it, which
// is the gesture the shape asks for, with the arrows kept as a precise
// alternative. It wraps rather than stopping at the ends: with a handful of
// buttons, a hard stop just means dragging back the way you came.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <digitiz/core/geometry.hpp>
#include <digitiz/proto/messages.hpp>

#include "buttons/ButtonStore.hpp"
#include "render/UiRenderer.hpp"
#include "text/TextRenderer.hpp"

namespace digitiz::guest {

enum class StripOrientation : std::uint8_t { Horizontal, Vertical };

class ButtonStrip {
public:
    // Borrowed, not copied: the store and the strip both belong to the render
    // thread, and a copy would only be a second thing to keep in step.
    void set_buttons(const std::vector<CustomButton>* buttons) noexcept { buttons_ = buttons; }

    // How many buttons the strip is allowed to show at once. Zero asks for
    // the default, which is the most that fit in half the edge -- a strip is
    // an overlay on a drawing surface, and one that runs the whole way across
    // by default would be taking more than it asked for. Clamped to what the
    // screen can actually hold.
    void set_slot_limit(int slots) noexcept { slot_limit_ = slots; }
    int slot_limit() const noexcept { return effective_limit_; }
    int max_slots() const noexcept { return max_slots_; }

    void set_orientation(StripOrientation o) noexcept { orientation_ = o; }
    StripOrientation orientation() const noexcept { return orientation_; }
    void set_expanded(bool on) noexcept { expanded_ = on; }
    bool expanded() const noexcept { return expanded_; }

    void layout(int surface_w, int surface_h, float density);
    void advance(double dt_seconds);

    bool hit_test(core::Vec2 p);
    void drag(core::Vec2 p);
    void release(core::Vec2 p);
    // Abandons a press without acting on it.
    void cancel_press();

    void draw(UiRenderer& ui) const;
    void draw_labels(TextRenderer& text) const;
    void load_labels(TextRenderer& text);

    // The PC program in focus, shown beside the strip. It is the answer to
    // "why these buttons", so it has to be readable at the same time as them --
    // which is why it is here and not in the side menu drawer.
    void set_active_window(std::string process);

    // What the strip covers, so the minimap can stay out of the way.
    Rect occupied() const;

    // Pulled once per frame by the caller.
    bool take_add_request() noexcept;
    bool take_state_change() noexcept; // orientation or collapsed state
    int take_long_press() noexcept;    // button index, or -1

    // Offsets inside the host's focused window, not desktop pixels; the caller
    // stamps them and sets the window-relative flag.
    using PointerSink = std::function<void(proto::PointerAction, core::Vec2 target)>;
    using ShortcutSink = std::function<void(const CustomButton&)>;
    void set_sinks(PointerSink pointer, ShortcutSink shortcut) {
        pointer_ = std::move(pointer);
        shortcut_ = std::move(shortcut);
    }

private:
    // What the finger came down on. Everything acts on release, so that a hold
    // can still turn out to be a long press and a slide can still turn out to
    // be a scroll.
    enum class Press : std::uint8_t { None, Toggle, Prev, Next, Add, Slot };

    struct Metrics {
        float margin = 0.0f;
        float thickness = 0.0f;
        float toggle = 0.0f;
        float arrow = 0.0f;
        float add = 0.0f;
        float slot = 0.0f;
        int visible = 0;    // slots that fit in the run
        bool arrows = false;
        float run = 0.0f;   // length of the scrolling area
        float start = 0.0f; // main-axis origin of the whole strip
        float cross = 0.0f; // cross-axis origin
    };

    bool horizontal() const noexcept { return orientation_ == StripOrientation::Horizontal; }
    // Builds a rect from a main-axis offset and length, in whichever direction
    // the strip is running. Everything below is laid out through this, so the
    // two orientations share one set of arithmetic.
    Rect cell(float offset, float length) const;
    Rect toggle_rect() const;
    Rect prev_rect() const;
    Rect next_rect() const;
    Rect add_rect() const;
    Rect run_rect() const;
    float run_start() const;
    // The k-th drawn position, which slides with the scroll and so is not tied
    // to any particular button.
    Rect slot_rect(int k) const;
    // The touchable model of a region button's rectangle, letterboxed inside
    // the slot so its shape matches the PC area it stands for.
    Rect region_pad(const Rect& slot, const CustomButton& button) const;
    core::Vec2 caption_origin() const;

    int count() const noexcept {
        return buttons_ == nullptr ? 0 : static_cast<int>(buttons_->size());
    }
    bool scrollable() const noexcept { return m_.arrows; }
    float content_length() const noexcept { return static_cast<float>(count()) * m_.slot; }
    // Scroll wrapped into one lap of the content, so the drawn window is
    // always somewhere sensible however far the raw offset has run.
    float wrapped_scroll() const;
    // Which button lands in the k-th drawn slot, or -1.
    int button_at(int k) const;
    const CustomButton* pressed_button() const;
    void nudge(int slots);
    void snap_to_slot();
    void activate(const CustomButton& button, core::Vec2 at);
    core::Vec2 map_into_region(const CustomButton& button, const Rect& pad, core::Vec2 p) const;
    void draw_slot(UiRenderer& ui, int k) const;
    // Distance along the strip, signed the way the content moves.
    double along(core::Vec2 a, core::Vec2 b) const;

    const std::vector<CustomButton>* buttons_ = nullptr;
    StripOrientation orientation_ = StripOrientation::Horizontal;
    bool expanded_ = false;

    int surface_w_ = 0;
    int surface_h_ = 0;
    float density_ = 1.0f;
    Metrics m_;

    int slot_limit_ = 0;      // 0 asks for the default
    int effective_limit_ = 1; // after the default and the clamp
    int max_slots_ = 1;       // what this screen can hold in this direction

    // Pixels, not an index: a drag has to move the list by however far the
    // finger went, not in whole buttons. Never wrapped in place, so an
    // animation across the seam stays monotonic; wrapped_scroll() folds it.
    double scroll_ = 0.0;
    double scroll_target_ = 0.0;
    double scroll_at_press_ = 0.0;

    Press press_ = Press::None;
    int press_slot_ = -1; // absolute index into the store
    core::Vec2 press_pos_{};
    double press_seconds_ = 0.0;
    bool press_moved_ = false;
    bool scrolling_ = false;

    bool add_requested_ = false;
    bool state_changed_ = false;
    int long_press_ = -1;

    PointerSink pointer_;
    ShortcutSink shortcut_;

    std::string active_process_;
    std::string caption_label_;
    std::string caption_none_;
    bool labels_loaded_ = false;
};

} // namespace digitiz::guest
