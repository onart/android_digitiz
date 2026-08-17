#include "input/TouchRouter.hpp"

#include <chrono>
#include <cmath>

#include <android/input.h>
#include <game-activity/GameActivity.h>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

core::Vec2 pointer_pos(const GameActivityMotionEvent& event, std::uint32_t index) {
    return core::Vec2{
        static_cast<double>(GameActivityPointerAxes_getX(&event.pointers[index])),
        static_cast<double>(GameActivityPointerAxes_getY(&event.pointers[index])),
    };
}

core::Vec2 centroid_of(const GameActivityMotionEvent& event) {
    core::Vec2 sum{};
    for (std::uint32_t i = 0; i < event.pointerCount; ++i) {
        sum = sum + pointer_pos(event, i);
    }
    const double n = static_cast<double>(event.pointerCount);
    return n > 0.0 ? sum / n : sum;
}

// Mean distance from the centroid. Works for any finger count, unlike the
// usual two-finger distance.
double spread_of(const GameActivityMotionEvent& event, core::Vec2 centroid) {
    if (event.pointerCount < 2) {
        return 0.0;
    }
    double total = 0.0;
    for (std::uint32_t i = 0; i < event.pointerCount; ++i) {
        const core::Vec2 d = pointer_pos(event, i) - centroid;
        total += std::sqrt(d.x * d.x + d.y * d.y);
    }
    return total / static_cast<double>(event.pointerCount);
}

std::int32_t pointer_index_of(std::int32_t action) {
    return (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
           AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
}

} // namespace

void TouchRouter::handle(const GameActivityMotionEvent& event) {
    const std::int32_t action = event.action & AMOTION_EVENT_ACTION_MASK;

    // GameActivityMotionEvent::eventTime is in *milliseconds* on the same
    // monotonic clock as steady_clock, despite the nanosecond units the
    // underlying AMotionEvent API uses. Measured, not assumed — see the
    // sanity check in emit().
    const std::uint64_t t_us = static_cast<std::uint64_t>(event.eventTime) * 1000ull;

    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN: {
        const core::Vec2 p = pointer_pos(event, 0);

        // A widget swallowed it; do not also draw on the PC.
        if (ui_hit_ && ui_hit_(p)) {
            suppress_until_release_ = true;
            return;
        }

        stroke_active_ = true;
        stroke_pointer_id_ = event.pointers[0].id;
        last_sent_ = p;
        last_sent_us_ = t_us;
        emit(proto::PointerAction::Down, p, t_us);
        break;
    }

    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        // Second finger: this was a pinch all along.
        if (stroke_active_) {
            emit(proto::PointerAction::Cancel, last_sent_, t_us);
            stroke_active_ = false;
            stroke_pointer_id_ = -1;
        }
        suppress_until_release_ = true;
        begin_gesture(event);
        break;
    }

    case AMOTION_EVENT_ACTION_MOVE: {
        if (gesture_active_ && event.pointerCount >= 2) {
            update_gesture(event);
            break;
        }
        if (!stroke_active_) {
            break;
        }

        // Find our pointer; on a multi-touch event it may not be index 0.
        for (std::uint32_t i = 0; i < event.pointerCount; ++i) {
            if (event.pointers[i].id != stroke_pointer_id_) {
                continue;
            }
            const core::Vec2 p = pointer_pos(event, i);

            // Milestone 2 throttling; both thresholds are 0 for now.
            const core::Vec2 d = p - last_sent_;
            const double moved = std::sqrt(d.x * d.x + d.y * d.y);
            const bool far_enough = moved >= min_distance_px_;
            const bool late_enough = t_us - last_sent_us_ >= min_interval_us_;
            if (!far_enough || !late_enough) {
                break;
            }

            last_sent_ = p;
            last_sent_us_ = t_us;
            emit(proto::PointerAction::Move, p, t_us);
            break;
        }
        break;
    }

    case AMOTION_EVENT_ACTION_POINTER_UP: {
        // Back down to one finger, but do not resume drawing with it: the user
        // is finishing a pinch, not starting a stroke.
        if (event.pointerCount <= 2) {
            gesture_active_ = false;
        } else {
            begin_gesture(event); // re-baseline without the lifted finger
        }
        break;
    }

    case AMOTION_EVENT_ACTION_UP: {
        if (stroke_active_) {
            const std::int32_t index = pointer_index_of(event.action);
            const core::Vec2 p =
                pointer_pos(event, static_cast<std::uint32_t>(index >= 0 ? index : 0));
            emit(proto::PointerAction::Up, p, t_us);
        }
        stroke_active_ = false;
        gesture_active_ = false;
        suppress_until_release_ = false;
        stroke_pointer_id_ = -1;
        break;
    }

    case AMOTION_EVENT_ACTION_CANCEL: {
        cancel_stroke();
        gesture_active_ = false;
        suppress_until_release_ = false;
        break;
    }

    default:
        break;
    }
}

void TouchRouter::cancel_stroke() {
    if (!stroke_active_) {
        return;
    }
    emit(proto::PointerAction::Cancel, last_sent_, last_sent_us_);
    stroke_active_ = false;
    stroke_pointer_id_ = -1;
}

void TouchRouter::begin_gesture(const GameActivityMotionEvent& event) {
    gesture_active_ = true;
    gesture_centroid_ = centroid_of(event);
    gesture_spread_ = spread_of(event, gesture_centroid_);
}

void TouchRouter::update_gesture(const GameActivityMotionEvent& event) {
    const core::Vec2 centroid = centroid_of(event);
    const double spread = spread_of(event, centroid);

    // Pan first, so the zoom anchor is evaluated in the already-panned frame.
    view_->pan_by(centroid - gesture_centroid_);

    if (gesture_spread_ > 1.0 && spread > 1.0) {
        view_->zoom_about(centroid, spread / gesture_spread_);
    }

    gesture_centroid_ = centroid;
    gesture_spread_ = spread;
}

void TouchRouter::emit(proto::PointerAction action, core::Vec2 surface, std::uint64_t t_us) {
    if (!sink_) {
        return;
    }

    // The host translates this timestamp onto its own clock using an offset
    // measured from PONG replies, which are stamped with steady_clock. That is
    // only meaningful if motion events share that clock and unit, so verify it
    // once per session instead of trusting the header. Getting the unit wrong
    // does not fail loudly — latency simply reads as nonsense and gets
    // discarded — so this check is what makes the mistake visible.
    static bool clock_checked = false;
    if (!clock_checked) {
        clock_checked = true;
        const auto steady_us = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        const double skew_ms = static_cast<double>(steady_us - static_cast<std::int64_t>(t_us)) /
                               1000.0;
        if (skew_ms < -50.0 || skew_ms > 500.0) {
            DZ_WARN("motion event clock looks wrong: skew %.1f ms (steady %lld us, event %llu us)."
                    " Latency figures will be discarded.",
                    skew_ms, static_cast<long long>(steady_us),
                    static_cast<unsigned long long>(t_us));
        } else {
            DZ_INFO("motion event clock agrees with steady_clock, skew %.1f ms", skew_ms);
        }
    }
    const core::Vec2 pc = view_->to_pc(surface);

    proto::Pointer msg;
    msg.t_us = t_us;
    msg.x = static_cast<std::int32_t>(std::lround(pc.x));
    msg.y = static_cast<std::int32_t>(std::lround(pc.y));
    msg.action = action;
    msg.button = proto::MouseButton::Left;
    msg.pointer_id = 0;
    msg.pressure = 1.0f;
    sink_(msg);
}

} // namespace digitiz::guest
