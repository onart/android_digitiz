#pragma once

// The row (or column) of custom buttons.
//
// Lives on one screen edge and collapses to a tab, because every pixel it
// occupies is a pixel that cannot be drawn on. When there are more buttons
// than fit, the arrows rotate through them rather than scrolling: a fixed set
// of slots keeps a button in the same place from one look to the next, which
// a scroll position does not.

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

    void set_orientation(StripOrientation o) noexcept { orientation_ = o; }
    StripOrientation orientation() const noexcept { return orientation_; }
    void set_expanded(bool on) noexcept { expanded_ = on; }
    bool expanded() const noexcept { return expanded_; }

    void layout(int surface_w, int surface_h, float density);
    void advance(double dt_seconds);

    bool hit_test(core::Vec2 p);
    void drag(core::Vec2 p);
    void release(core::Vec2 p);
    // Abandons a press without acting on it. A region drag in flight has the
    // host holding a mouse button, so that one cannot simply be forgotten.
    void cancel_press();

    void draw(UiRenderer& ui) const;
    void draw_labels(TextRenderer& text) const;
    void load_labels(TextRenderer& text);

    // The PC program in focus, shown beside the strip. It is the answer to
    // "why these buttons", so it has to be readable at the same time as them —
    // which is why it is here and not in the side menu drawer.
    void set_active_window(std::string process);

    // What the strip covers, so the minimap can stay out of the way.
    Rect occupied() const;

    // Pulled once per frame by the caller.
    bool take_add_request() noexcept;
    bool take_state_change() noexcept; // orientation or collapsed state
    int take_long_press() noexcept;    // button index, or -1

    // Absolute PC coordinates; the caller stamps and sends them.
    using PointerSink = std::function<void(proto::PointerAction, core::Vec2 pc)>;
    using ShortcutSink = std::function<void(const CustomButton&)>;
    void set_sinks(PointerSink pointer, ShortcutSink shortcut) {
        pointer_ = std::move(pointer);
        shortcut_ = std::move(shortcut);
    }

private:
    // What the finger came down on. Everything acts on release, so that a hold
    // can still turn out to be a long press instead.
    enum class Press : std::uint8_t { None, Toggle, Prev, Next, Add, Slot };

    struct Metrics {
        float margin = 0.0f;
        float thickness = 0.0f;
        float toggle = 0.0f;
        float arrow = 0.0f;
        float add = 0.0f;
        float slot = 0.0f;
        int visible = 0;   // slots actually drawn
        bool arrows = false;
        float start = 0.0f; // main-axis origin of the run
        float cross = 0.0f; // cross-axis origin of the strip
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
    Rect slot_rect(int slot) const;
    // The touchable model of a region button's rectangle, letterboxed inside
    // the slot so its shape matches the PC area it stands for.
    Rect region_pad(const Rect& slot, const CustomButton& button) const;
    core::Vec2 caption_origin() const;

    int count() const noexcept { return buttons_ == nullptr ? 0 : static_cast<int>(buttons_->size()); }
    // Which button sits in a given slot, or -1.
    int button_at(int slot) const;
    const CustomButton* pressed_button() const;
    void cycle(int delta);
    void activate(const CustomButton& button, core::Vec2 at);
    core::Vec2 map_into_region(const CustomButton& button, const Rect& pad, core::Vec2 p) const;
    void emit_region(proto::PointerAction action, core::Vec2 pc);
    void draw_slot(UiRenderer& ui, int slot) const;

    const std::vector<CustomButton>* buttons_ = nullptr;
    StripOrientation orientation_ = StripOrientation::Horizontal;
    bool expanded_ = false;

    int surface_w_ = 0;
    int surface_h_ = 0;
    float density_ = 1.0f;
    Metrics m_;

    int first_ = 0; // index of the button in the leftmost/topmost slot

    Press press_ = Press::None;
    int press_slot_ = -1; // absolute index into the store
    core::Vec2 press_pos_{};
    double press_seconds_ = 0.0;
    bool press_moved_ = false;
    bool region_dragging_ = false;
    // Where the last region event was aimed. A cancel has to be delivered
    // somewhere, and the host releases the mouse button at whatever position
    // the message carries — sending a default would drop it in the corner of
    // the desktop rather than where the finger was.
    core::Vec2 last_region_pc_{};

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
