#include "input/PointerPipeline.hpp"

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

void PointerPipeline::handle(const proto::Pointer& p) {
    ++stats_.received;

    if (!enabled_) {
        ++stats_.dropped_disabled;
        return;
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
        break;

    case proto::PointerAction::Move:
    case proto::PointerAction::Hover:
        inject_move(p.x, p.y);
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
        inject_button(held_, false, p.x, p.y);
        down_ = false;
        break;

    case proto::PointerAction::Cancel:
        // The guest saw a second finger land and switched to view manipulation.
        // Abandon the stroke where it stands.
        if (down_) {
            inject_button(held_, false, p.x, p.y);
            down_ = false;
        }
        break;
    }
}

void PointerPipeline::end_session() {
    release_stroke();
    injector_->release_all(); // belt and braces: catches anything we lost track of
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

bool PointerPipeline::on_screen(std::int32_t x, std::int32_t y) const {
    // No test configured means no display information yet; let it through and
    // rely on the injector's clamp rather than blocking everything.
    return !on_screen_ || on_screen_(x, y);
}

void PointerPipeline::release_stroke() {
    if (!down_) {
        return;
    }
    DZ_WARN("releasing %s held at (%d, %d)", proto::to_string(held_), last_x_, last_y_);
    inject_button(held_, false, last_x_, last_y_);
    down_ = false;
}

} // namespace digitiz::host
