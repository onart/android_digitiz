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
    // One finger draws on the PC, two or more move the view.
    Draw,
    // Every finger moves the view and nothing is sent. Lets the view be placed
    // precisely with one thumb, without a stray stroke landing on the PC.
    Pan,
};

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

    // Pointer events consumed by widgets before routing, so a tap on the side
    // menu does not also draw on the PC.
    void set_ui_hit_test(std::function<bool(core::Vec2)> hit) { ui_hit_ = std::move(hit); }

private:
    void emit(proto::PointerAction action, core::Vec2 surface, std::uint64_t t_us);
    // `exclude` is the index of a pointer that is lifting and must be ignored.
    void begin_gesture(const GameActivityMotionEvent& event, std::int32_t exclude = -1);
    void update_gesture(const GameActivityMotionEvent& event, std::int32_t exclude = -1);

    core::ViewTransform* view_;
    PointerSink sink_;
    std::function<bool(core::Vec2)> ui_hit_;

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
};

} // namespace digitiz::guest
