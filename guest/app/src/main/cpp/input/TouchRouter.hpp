#pragma once

// Decides what a touch means.
//
//   one finger   -> digitizer input, forwarded to the PC
//   two or more  -> view manipulation (pinch zoom / pan), stays local
//
// The first finger cannot be told apart from the start of a pinch, and waiting
// to find out would put a delay on every stroke. So a stroke starts
// immediately, and if a second finger lands the stroke is CANCELled and the
// host releases the button. This is how Android's own gesture handling works.

#include <cstdint>
#include <functional>

#include <digitiz/core/geometry.hpp>
#include <digitiz/proto/messages.hpp>

struct GameActivityMotionEvent;

namespace digitiz::guest {

enum class InputMode : std::uint8_t {
    // Pen: the surface maps onto the desktop one to one, and touching presses.
    Draw,
    // Trackpad: only the scale survives. A finger moves the cursor from
    // wherever it already is, without pressing, and a short tap clicks. Lets
    // the finger be lifted and replaced without the cursor jumping.
    Slide,
    // View only. Every finger moves the view and nothing is sent.
    Pan,
};

// Cycles Draw -> Slide -> Pan -> Draw.
InputMode next_mode(InputMode mode) noexcept;

class TouchRouter {
public:
    using PointerSink = std::function<void(const proto::Pointer&)>;

    TouchRouter(core::ViewTransform& view, PointerSink sink)
        : view_(&view), sink_(std::move(sink)) {}

    void handle(const GameActivityMotionEvent& event);

    // Switching away from Draw mid-stroke releases on the host first.
    void set_mode(InputMode mode);
    InputMode mode() const noexcept { return mode_; }

    // Drops any stroke in progress, telling the host to release. Call when the
    // link goes down or the app loses focus.
    void cancel_stroke();

    bool stroke_active() const noexcept { return stroke_active_; }
    bool view_gesture_active() const noexcept { return gesture_active_; }

    // True once the user has pinched or panned. Until then the view is still
    // whatever was auto-framed, and is free to be re-framed on a rotation.
    bool view_adjusted_by_user() const noexcept { return view_adjusted_; }

    // Milestone 2 hooks: throttle in time and space to smooth drawn curves.
    // Zero means "send everything", which is the milestone 1 behaviour.
    void set_min_interval_us(std::uint64_t us) noexcept { min_interval_us_ = us; }
    void set_min_distance_px(double px) noexcept { min_distance_px_ = px; }

    // Scales the tap thresholds used in Slide mode.
    void set_density(float density) noexcept { density_ = density > 0.0f ? density : 1.0f; }

    // Pointer events consumed by widgets before routing, so a tap on the side
    // menu does not also draw on the PC.
    void set_ui_hit_test(std::function<bool(core::Vec2)> hit) { ui_hit_ = std::move(hit); }

    // Answers whether a PC-space point actually lands on a screen. The union
    // of the monitors, not the bounding box: two monitors of different heights
    // leave a corner inside the box that belongs to neither, and a touch there
    // has nowhere to go.
    void set_pc_point_test(std::function<bool(core::Vec2)> on_screen) {
        on_screen_ = std::move(on_screen);
    }

private:
    void emit(proto::PointerAction action, core::Vec2 surface, std::uint64_t t_us);
    // Sends a delta rather than a position. The first call of a gesture is
    // marked so the host re-reads the real cursor before accumulating.
    void emit_relative(proto::PointerAction action, std::int32_t dx, std::int32_t dy,
                       std::uint64_t t_us);
    void slide_begin(core::Vec2 surface, std::uint64_t t_us);
    void slide_move(core::Vec2 surface, std::uint64_t t_us);
    void slide_end(core::Vec2 surface, std::uint64_t t_us);
    // `exclude` is the index of a pointer that is lifting and must be ignored.
    void begin_gesture(const GameActivityMotionEvent& event, std::int32_t exclude = -1);
    void update_gesture(const GameActivityMotionEvent& event, std::int32_t exclude = -1);

    bool lands_on_screen(core::Vec2 surface) const;

    core::ViewTransform* view_;
    PointerSink sink_;
    std::function<bool(core::Vec2)> ui_hit_;
    std::function<bool(core::Vec2)> on_screen_;

    InputMode mode_ = InputMode::Draw;
    bool stroke_active_ = false;
    bool gesture_active_ = false;
    bool view_adjusted_ = false;
    std::int32_t stroke_pointer_id_ = -1;

    core::Vec2 last_sent_{};
    std::uint64_t last_sent_us_ = 0;
    std::uint64_t min_interval_us_ = 0;
    double min_distance_px_ = 0.0;

    core::Vec2 gesture_centroid_{};
    double gesture_spread_ = 0.0;

    // Slide mode. The remainder matters: at a scale of 0.5 a one pixel swipe
    // is half a PC pixel, and truncating each event would make slow movement
    // vanish entirely.
    float density_ = 1.0f;
    bool slide_active_ = false;
    bool slide_started_sent_ = false;
    core::Vec2 slide_last_{};
    core::Vec2 slide_remainder_{};
    core::Vec2 slide_origin_{};
    std::uint64_t slide_origin_us_ = 0;
    double slide_travel_ = 0.0;
    std::int32_t slide_sent_x_ = 0;
    std::int32_t slide_sent_y_ = 0;
    int slide_sent_events_ = 0;
};

} // namespace digitiz::guest
