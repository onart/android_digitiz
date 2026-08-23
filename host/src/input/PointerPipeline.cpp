#include "input/PointerPipeline.hpp"

#include <cmath>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

void PointerPipeline::set_enabled(bool on) {
    if (enabled_ == on) {
        return;
    }
    if (!on) {
        // Never leave a button held across a disable.
        release_stroke();
    }
    enabled_ = on;
    DZ_INFO("injection %s", on ? "ENABLED" : "DISABLED");
}

void PointerPipeline::handle(const proto::Pointer& in) {
    ++stats_.received;

    if (!enabled_) {
        ++stats_.dropped_disabled;
        return;
    }

    if ((in.flags & proto::kPointerRelative) != 0) {
        ++stats_.relative_events;
        handle_relative(in);
        return;
    }

    proto::Pointer p = in;
    if ((p.flags & proto::kPointerWindowRelative) != 0) {
        core::Recti window{};
        if (!window_bounds_ || !window_bounds_(window)) {
            // Nothing has the focus, or it refuses to be measured. Dropped
            // rather than fallen back to treating the offset as a desktop
            // coordinate, which would click somewhere unrelated.
            ++stats_.dropped_no_window;
            return;
        }
        p.x += window.x;
        p.y += window.y;
    }

    // UP and CANCEL are always honoured wherever they land: refusing to
    // release is how a button gets stuck on the user's desktop.
    const bool needs_screen =
        p.action == proto::PointerAction::Down || p.action == proto::PointerAction::Move ||
        p.action == proto::PointerAction::Hover;
    if (needs_screen && !on_screen(p.x, p.y)) {
        ++stats_.dropped_off_screen;
        return;
    }

    last_x_ = p.x;
    last_y_ = p.y;

    switch (p.action) {
    case proto::PointerAction::Down:
        if (down_) {
            // Guest lost an UP somewhere. Close the old stroke before opening
            // a new one, or the button stays held forever.
            ++stats_.protocol_errors;
            DZ_WARN("pointer: DOWN while %s already held; closing previous stroke",
                    proto::to_string(held_));
            inject_button(held_, false, p.x, p.y);
        }
        inject_button(p.button, true, p.x, p.y);
        down_ = true;
        held_ = p.button;
        // The press already put the cursor here, so this seeds the curve
        // without emitting anything.
        smoother_.begin(p.x, p.y);
        break;

    case proto::PointerAction::Move:
    case proto::PointerAction::Hover:
        // Bypassed rather than run in passthrough when off, so the plain path
        // is provably the one that existed before smoothing was added.
        if (smoother_.enabled()) {
            smoother_.add(p.x, p.y, [this](double sx, double sy) { emit_smoothed(sx, sy); });
        } else {
            inject_move(p.x, p.y);
        }
        break;

    case proto::PointerAction::Up:
        if (!down_) {
            ++stats_.protocol_errors;
            DZ_WARN("pointer: UP with no stroke open; ignoring");
            break;
        }
        if (p.button != held_) {
            // Trust our own record over the message: releasing the button we
            // actually pressed is what unsticks the mouse.
            ++stats_.protocol_errors;
            DZ_WARN("pointer: UP for %s but %s is held; releasing %s",
                    proto::to_string(p.button), proto::to_string(held_),
                    proto::to_string(held_));
        }
        // Drain the curve before releasing, or the stroke would stop one
        // sample short of where the finger actually lifted. Skipped entirely
        // when smoothing is off: the release already carries the position, and
        // an extra move would be a behaviour change for the plain path.
        if (smoother_.enabled()) {
            const auto emit = [this](double sx, double sy) { emit_smoothed(sx, sy); };
            smoother_.add(p.x, p.y, emit);
            smoother_.finish(emit);
        }
        inject_button(held_, false, p.x, p.y);
        down_ = false;
        break;

    case proto::PointerAction::Cancel:
        // The guest saw a second finger land and switched to view manipulation.
        // Abandon the stroke where it stands.
        smoother_.reset();
        if (down_) {
            inject_button(held_, false, p.x, p.y);
            down_ = false;
        }
        break;
    }
}

void PointerPipeline::handle(const proto::Key& k) {
    ++stats_.received;

    if (!enabled_) {
        ++stats_.dropped_disabled;
        return;
    }

    if (!injector_->key(k)) {
        // The injector has already said why; counted separately from a pointer
        // failure because the usual cause is different — a key name we do not
        // know, rather than a window we may not touch.
        ++stats_.keys_unknown;
        return;
    }
    ++stats_.keys_sent;
    ++stats_.injected;
}

void PointerPipeline::end_session() {
    // The next session starts against whatever the cursor is doing then.
    rel_seeded_ = false;
    release_stroke();
    injector_->release_all(); // belt and braces: catches anything we lost track of
}

void PointerPipeline::emit_smoothed(double x, double y) {
    ++stats_.smoothed_points;
    inject_move(static_cast<std::int32_t>(std::lround(x)),
                static_cast<std::int32_t>(std::lround(y)));
}

void PointerPipeline::inject_move(std::int32_t x, std::int32_t y) {
    if (injector_->move_to(x, y)) {
        ++stats_.injected;
    } else {
        ++stats_.inject_failures;
    }
}

void PointerPipeline::inject_button(proto::MouseButton b, bool down, std::int32_t x,
                                    std::int32_t y) {
    if (injector_->button(b, down, x, y)) {
        ++stats_.injected;
    } else {
        ++stats_.inject_failures;
    }
}

void PointerPipeline::handle_relative(const proto::Pointer& p) {
    // The guest has no idea where the cursor is, and the user may have nudged
    // a physical mouse since the last gesture, so start from the truth.
    if ((p.flags & proto::kPointerGestureStart) != 0 || !rel_seeded_) {
        std::int32_t cx = 0;
        std::int32_t cy = 0;
        if (injector_->cursor_pos(cx, cy)) {
            rel_x_ = cx;
            rel_y_ = cy;
        }
        rel_seeded_ = true;
    }

    advance_relative(p.x, p.y);

    const auto x = static_cast<std::int32_t>(std::lround(rel_x_));
    const auto y = static_cast<std::int32_t>(std::lround(rel_y_));
    last_x_ = x;
    last_y_ = y;

    switch (p.action) {
    case proto::PointerAction::Down:
        if (down_) {
            ++stats_.protocol_errors;
            inject_button(held_, false, x, y);
        }
        inject_button(p.button, true, x, y);
        down_ = true;
        held_ = p.button;
        break;

    case proto::PointerAction::Move:
    case proto::PointerAction::Hover:
        inject_move(x, y);
        break;

    case proto::PointerAction::Up:
        if (!down_) {
            // Normal in slide mode: moving the cursor presses nothing.
            break;
        }
        inject_button(held_, false, x, y);
        down_ = false;
        break;

    case proto::PointerAction::Cancel:
        if (down_) {
            inject_button(held_, false, x, y);
            down_ = false;
        }
        break;
    }
}

void PointerPipeline::advance_relative(double dx, double dy) {
    if (!on_screen_) {
        rel_x_ += dx;
        rel_y_ += dy;
        return;
    }

    const auto lands = [this](double x, double y) {
        return on_screen_(static_cast<std::int32_t>(std::lround(x)),
                          static_cast<std::int32_t>(std::lround(y)));
    };

    const double want_x = rel_x_ + dx;
    const double want_y = rel_y_ + dy;

    if (lands(want_x, want_y)) {
        rel_x_ = want_x;
        rel_y_ = want_y;
        return;
    }
    // Refused as a pair — try each axis so a diagonal drag into an edge keeps
    // sliding along it. Clamping the tracked position rather than only the
    // injected one matters: otherwise coming back requires first unwinding
    // however far past the edge the accumulator ran.
    if (lands(want_x, rel_y_)) {
        rel_x_ = want_x;
        return;
    }
    if (lands(rel_x_, want_y)) {
        rel_y_ = want_y;
    }
}

bool PointerPipeline::on_screen(std::int32_t x, std::int32_t y) const {
    // No test configured means no display information yet; let it through and
    // rely on the injector's clamp rather than blocking everything.
    return !on_screen_ || on_screen_(x, y);
}

void PointerPipeline::release_stroke() {
    // Whatever the curve still holds belongs to a stroke that is being torn
    // down, so it is dropped rather than drawn.
    smoother_.reset();
    if (!down_) {
        return;
    }
    DZ_WARN("releasing %s held at (%d, %d)", proto::to_string(held_), last_x_, last_y_);
    inject_button(held_, false, last_x_, last_y_);
    down_ = false;
}

} // namespace digitiz::host
