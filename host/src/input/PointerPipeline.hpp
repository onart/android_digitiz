#pragma once

// Turns a stream of POINTER messages into injector calls.
//
// The guest is not trusted to send a well-formed sequence: packets can be lost
// on a reconnect, the app can be killed mid-stroke, and a buggy build can send
// nonsense. Every path here ends with the mouse button released, because the
// failure mode that actually hurts the user is a button stuck down on their PC.

#include <cstdint>
#include <functional>

#include <digitiz/core/geometry.hpp>
#include <digitiz/proto/messages.hpp>

#include "input/InputInjector.hpp"
#include "input/SplineSmoother.hpp"

namespace digitiz::host {

class PointerPipeline {
public:
    struct Stats {
        std::uint64_t received = 0;
        std::uint64_t injected = 0;
        std::uint64_t dropped_disabled = 0;
        std::uint64_t protocol_errors = 0; // duplicate DOWN, UP with no DOWN, ...
        std::uint64_t inject_failures = 0;
        std::uint64_t dropped_off_screen = 0;
        std::uint64_t relative_events = 0;
        // Points the smoother produced from those samples.
        std::uint64_t smoothed_points = 0;
        std::uint64_t keys_sent = 0;
        std::uint64_t keys_unknown = 0;
        std::uint64_t dropped_no_window = 0;
    };

    // Curve fitting between samples. Absolute strokes only — in slide mode it
    // would put lag on plain pointing, which is the opposite of useful.
    SplineSmoother& smoother() noexcept { return smoother_; }
    const SplineSmoother& smoother() const noexcept { return smoother_; }

    // Points that land on no display are ignored rather than clamped. The
    // guest filters these too, but it is not trusted to: a stale monitor list
    // on its side must not move the user's cursor.
    using ScreenTest = std::function<bool(std::int32_t, std::int32_t)>;
    void set_screen_test(ScreenTest test) { on_screen_ = std::move(test); }

    // Where the focused window is, for pointers flagged window-relative.
    // Returning false drops the event: a button that cannot be placed must not
    // be placed approximately.
    using WindowBounds = std::function<bool(core::Recti&)>;
    void set_window_bounds(WindowBounds bounds) { window_bounds_ = std::move(bounds); }

    explicit PointerPipeline(IInputInjector& injector) : injector_(&injector) {}

    // Turning this off mid-stroke releases the held button first.
    void set_enabled(bool on);
    bool enabled() const noexcept { return enabled_; }

    void handle(const proto::Pointer& p);

    // Shortcuts from the guest's custom buttons. Routed through here rather
    // than straight to the injector so the on/off switch means the same thing
    // for keys as it does for the pointer.
    void handle(const proto::Key& k);

    // Call on disconnect, app exit, or any other abrupt end.
    void end_session();

    bool stroke_active() const noexcept { return down_; }
    proto::MouseButton held_button() const noexcept { return held_; }
    std::int32_t last_x() const noexcept { return last_x_; }
    std::int32_t last_y() const noexcept { return last_y_; }

    const Stats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = {}; }

private:
    void inject_move(std::int32_t x, std::int32_t y);
    void inject_button(proto::MouseButton b, bool down, std::int32_t x, std::int32_t y);
    void release_stroke();
    bool on_screen(std::int32_t x, std::int32_t y) const;

    void handle_relative(const proto::Pointer& p);
    // Moves the tracked position by (dx, dy), refusing to leave the screens.
    // Each axis is tried on its own so running into an edge slides along it
    // instead of stopping dead.
    void advance_relative(double dx, double dy);

    void emit_smoothed(double x, double y);

    IInputInjector* injector_;
    ScreenTest on_screen_;
    WindowBounds window_bounds_;
    SplineSmoother smoother_;

    // Relative mode tracks the cursor itself. Kept as doubles so a slow drag
    // at a small scale still accumulates instead of rounding away every event.
    double rel_x_ = 0.0;
    double rel_y_ = 0.0;
    bool rel_seeded_ = false;
    bool enabled_ = false;
    bool down_ = false;
    proto::MouseButton held_ = proto::MouseButton::Left;
    std::int32_t last_x_ = 0;
    std::int32_t last_y_ = 0;
    Stats stats_;
};

} // namespace digitiz::host
