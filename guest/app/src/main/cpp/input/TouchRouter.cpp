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

// `exclude` skips the pointer that is lifting: on ACTION_POINTER_UP it is
// still in the array, and counting it would make the view jump at the moment
// a finger leaves.
core::Vec2 centroid_of(const GameActivityMotionEvent& event, std::int32_t exclude) {
    core::Vec2 sum{};
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < event.pointerCount; ++i) {
        if (static_cast<std::int32_t>(i) == exclude) {
            continue;
        }
        sum = sum + pointer_pos(event, i);
        ++n;
    }
    return n > 0 ? sum / static_cast<double>(n) : sum;
}

// Mean distance from the centroid. Works for any finger count, unlike the
// usual two-finger distance, and is 0 for a single finger so panning with one
// thumb does not accidentally zoom.
double spread_of(const GameActivityMotionEvent& event, core::Vec2 centroid, std::int32_t exclude) {
    double total = 0.0;
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < event.pointerCount; ++i) {
        if (static_cast<std::int32_t>(i) == exclude) {
            continue;
        }
        const core::Vec2 d = pointer_pos(event, i) - centroid;
        total += std::sqrt(d.x * d.x + d.y * d.y);
        ++n;
    }
    return n >= 2 ? total / static_cast<double>(n) : 0.0;
}

std::int32_t pointer_index_of(std::int32_t action) {
    return (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
           AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
}

// A touch counts as a tap only if it stayed put and was brief.
constexpr double kTapSlopDp = 10.0;
constexpr std::uint64_t kTapMaxUs = 300000;

} // namespace

InputMode next_mode(InputMode mode) noexcept {
    switch (mode) {
    case InputMode::Draw:
        return InputMode::Slide;
    case InputMode::Slide:
        return InputMode::Pan;
    case InputMode::Pan:
        break;
    }
    return InputMode::Draw;
}

void TouchRouter::set_mode(InputMode mode) {
    if (mode_ == mode) {
        return;
    }
    // Leaving Draw with a finger down would strand the button on the host.
    cancel_stroke();
    mode_ = mode;
    gesture_active_ = false;
    slide_active_ = false;

    const char* name = "draw";
    if (mode == InputMode::Slide) {
        name = "slide";
    } else if (mode == InputMode::Pan) {
        name = "pan";
    }
    DZ_INFO("input mode: %s", name);
}

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

        // A widget swallowed it and now owns this finger until it lifts. No
        // stroke is opened, so nothing reaches the PC either way.
        if (ui_hit_ && ui_hit_(p)) {
            ui_captured_ = true;
            return;
        }

        if (mode_ == InputMode::Pan) {
            begin_gesture(event);
            break;
        }

        if (mode_ == InputMode::Slide) {
            // Tracked by id like a stroke: a later MOVE carries every pointer,
            // and ours is not necessarily index 0.
            stroke_pointer_id_ = event.pointers[0].id;
            slide_begin(p, t_us);
            break;
        }

        // Nothing on the PC is under this point, so there is nothing to press.
        // Silently ignoring it beats clamping to the nearest screen edge,
        // which would jump the cursor somewhere the user never touched.
        if (!lands_on_screen(p)) {
            break;
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
        if (slide_active_) {
            // Abandon the slide; no button was pressed, so nothing to release.
            slide_active_ = false;
            stroke_pointer_id_ = -1;
        }
        begin_gesture(event);
        break;
    }

    case AMOTION_EVENT_ACTION_MOVE: {
        if (ui_captured_) {
            if (ui_drag_ && event.pointerCount > 0) {
                ui_drag_(pointer_pos(event, 0));
            }
            break;
        }
        if (gesture_active_) {
            update_gesture(event);
            break;
        }
        if (slide_active_) {
            for (std::uint32_t i = 0; i < event.pointerCount; ++i) {
                if (event.pointers[i].id == stroke_pointer_id_) {
                    slide_move(pointer_pos(event, i), t_us);
                    break;
                }
            }
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

            // Dragging off the edge holds the cursor at the last real position
            // rather than ending the stroke, so coming back continues the same
            // line instead of starting a new one.
            if (!lands_on_screen(p)) {
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
        const std::int32_t lifted = pointer_index_of(event.action);

        if (mode_ == InputMode::Pan) {
            // Every finger pans here, so keep going with whatever is left.
            begin_gesture(event, lifted);
            break;
        }

        // In Draw mode, dropping back to one finger ends the gesture but does
        // not resume drawing: the user is finishing a pinch, not starting a
        // stroke. Nothing extra is needed for that — the stroke was cancelled
        // when the second finger landed, and only ACTION_DOWN opens a new one.
        if (event.pointerCount <= 2) {
            gesture_active_ = false;
        } else {
            begin_gesture(event, lifted);
        }
        break;
    }

    case AMOTION_EVENT_ACTION_UP: {
        if (ui_captured_) {
            if (ui_release_ && event.pointerCount > 0) {
                ui_release_(pointer_pos(event, 0));
            }
            ui_captured_ = false;
            break;
        }
        if (slide_active_) {
            const std::int32_t index = pointer_index_of(event.action);
            slide_end(pointer_pos(event, static_cast<std::uint32_t>(index >= 0 ? index : 0)),
                      t_us);
        }
        if (stroke_active_) {
            const std::int32_t index = pointer_index_of(event.action);
            const core::Vec2 p =
                pointer_pos(event, static_cast<std::uint32_t>(index >= 0 ? index : 0));
            // Releasing off-screen still has to release: fall back to the last
            // point that was on a screen so the host never keeps the button
            // held, and never releases somewhere the cursor never was.
            emit(proto::PointerAction::Up, lands_on_screen(p) ? p : last_sent_, t_us);
        }
        stroke_active_ = false;
        gesture_active_ = false;
        slide_active_ = false;
        stroke_pointer_id_ = -1;
        break;
    }

    case AMOTION_EVENT_ACTION_CANCEL: {
        cancel_stroke();
        gesture_active_ = false;
        slide_active_ = false;
        if (ui_captured_) {
            if (ui_release_ && event.pointerCount > 0) {
                ui_release_(pointer_pos(event, 0));
            }
            ui_captured_ = false;
        }
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

void TouchRouter::begin_gesture(const GameActivityMotionEvent& event, std::int32_t exclude) {
    gesture_active_ = true;
    gesture_centroid_ = centroid_of(event, exclude);
    gesture_spread_ = spread_of(event, gesture_centroid_, exclude);
}

void TouchRouter::update_gesture(const GameActivityMotionEvent& event, std::int32_t exclude) {
    const core::Vec2 centroid = centroid_of(event, exclude);
    const double spread = spread_of(event, centroid, exclude);

    // Pan first, so the zoom anchor is evaluated in the already-panned frame.
    view_->pan_by(centroid - gesture_centroid_);

    if (gesture_spread_ > 1.0 && spread > 1.0) {
        view_->zoom_about(centroid, spread / gesture_spread_);
    }

    // From here on the view belongs to the user, and a rotation must not
    // silently undo what they set up.
    view_adjusted_ = true;

    gesture_centroid_ = centroid;
    gesture_spread_ = spread;
}

void TouchRouter::slide_begin(core::Vec2 surface, std::uint64_t t_us) {
    slide_active_ = true;
    slide_started_sent_ = false;
    slide_last_ = surface;
    slide_remainder_ = core::Vec2{};
    slide_origin_ = surface;
    slide_origin_us_ = t_us;
    slide_travel_ = 0.0;
    slide_sent_x_ = 0;
    slide_sent_y_ = 0;
    slide_sent_events_ = 0;
}

void TouchRouter::slide_move(core::Vec2 surface, std::uint64_t t_us) {
    const core::Vec2 step = surface - slide_last_;
    slide_last_ = surface;
    slide_travel_ += std::sqrt(step.x * step.x + step.y * step.y);

    // Surface pixels to PC pixels. The scale is what survives from the
    // absolute view: zoomed in means fine control, zoomed out means coarse.
    const double scale = view_->scale() > 0.0 ? view_->scale() : 1.0;
    const core::Vec2 wanted = step / scale + slide_remainder_;

    const double dx = std::trunc(wanted.x);
    const double dy = std::trunc(wanted.y);
    // Whatever did not reach a whole pixel is carried, not discarded.
    slide_remainder_ = core::Vec2{wanted.x - dx, wanted.y - dy};

    if (dx == 0.0 && dy == 0.0) {
        return;
    }
    slide_sent_x_ += static_cast<std::int32_t>(dx);
    slide_sent_y_ += static_cast<std::int32_t>(dy);
    ++slide_sent_events_;
    emit_relative(proto::PointerAction::Move, static_cast<std::int32_t>(dx),
                  static_cast<std::int32_t>(dy), t_us);
}

void TouchRouter::slide_end(core::Vec2 surface, std::uint64_t t_us) {
    slide_move(surface, t_us);

    const bool brief = t_us - slide_origin_us_ <= kTapMaxUs;
    const bool still = slide_travel_ <= kTapSlopDp * static_cast<double>(density_);
    slide_active_ = false;

    DZ_DEBUG("slide: %d event(s), total (%d, %d) px, %s", slide_sent_events_, slide_sent_x_,
             slide_sent_y_, (brief && still) ? "tap -> click" : "move only");

    if (!brief || !still) {
        return;
    }

    // A tap clicks wherever the cursor already is: press and release with no
    // movement of their own.
    emit_relative(proto::PointerAction::Down, 0, 0, t_us);
    emit_relative(proto::PointerAction::Up, 0, 0, t_us);
}

void TouchRouter::emit_relative(proto::PointerAction action, std::int32_t dx, std::int32_t dy,
                                std::uint64_t t_us) {
    if (!sink_) {
        return;
    }

    proto::Pointer msg;
    msg.t_us = t_us;
    msg.x = dx;
    msg.y = dy;
    msg.action = action;
    msg.button = proto::MouseButton::Left;
    msg.pointer_id = 0;
    msg.flags = proto::kPointerRelative;
    if (!slide_started_sent_) {
        msg.flags |= proto::kPointerGestureStart;
        slide_started_sent_ = true;
    }
    msg.pressure = 1.0f;
    sink_(msg);
}

bool TouchRouter::lands_on_screen(core::Vec2 surface) const {
    // Without a monitor list yet, allow everything: the host clamps, and
    // blocking input before the handshake would look like a dead app.
    if (!on_screen_) {
        return true;
    }
    return on_screen_(view_->to_pc(surface));
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
