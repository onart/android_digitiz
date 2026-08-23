#pragma once

// Catmull-Rom smoothing for an incoming stroke.
//
// The samples that arrive are sparse — more so with decimation turned on — and
// injecting them straight through draws a polyline. This fills each segment in
// with points along a Catmull-Rom curve instead.
//
// It costs exactly one sample of latency, and that cannot be engineered away:
// the curve between P1 and P2 is defined by P0 through P3, so P2's segment
// cannot be drawn until P3 has arrived. The cursor therefore trails the finger
// by one input interval while a stroke is in progress, and catches up when it
// ends. That is why this is optional.
//
// Centripetal parameterization (alpha 0.5) rather than uniform: our samples are
// unevenly spaced by construction, and uniform Catmull-Rom answers uneven
// spacing with cusps and self-intersecting loops.

#include <array>
#include <cstddef>
#include <functional>

namespace digitiz::host {

class SplineSmoother {
public:
    // Receives each point of the smoothed path, in order.
    using Emit = std::function<void(double x, double y)>;

    void set_enabled(bool on) noexcept { enabled_ = on; }
    bool enabled() const noexcept { return enabled_; }

    // Target spacing between emitted points, in pixels. Smaller is smoother
    // and more SendInput calls.
    void set_step_px(double px) noexcept { step_px_ = px > 0.25 ? px : 0.25; }
    double step_px() const noexcept { return step_px_; }

    // 0 uniform, 0.5 centripetal, 1 chordal.
    void set_alpha(double alpha) noexcept { alpha_ = alpha; }
    double alpha() const noexcept { return alpha_; }

    // Stroke start. The point itself is not emitted — the caller has already
    // put the cursor there to press the button.
    void begin(double x, double y);

    // A new sample. May emit nothing (still filling the window) or a run of
    // points covering the segment that just became defined.
    void add(double x, double y, const Emit& emit);

    // Stroke end: drains the window so the path finishes exactly on the last
    // sample received.
    void finish(const Emit& emit);

    void reset() noexcept { count_ = 0; }

    bool active() const noexcept { return count_ > 0; }

private:
    struct Point {
        double x = 0.0;
        double y = 0.0;
    };

    void push(Point p);
    // Emits the span between points_[1] and points_[2].
    void emit_segment(const Emit& emit) const;

    bool enabled_ = false;
    double step_px_ = 3.0;
    double alpha_ = 0.5;

    std::array<Point, 4> points_{};
    std::size_t count_ = 0;
};

} // namespace digitiz::host
