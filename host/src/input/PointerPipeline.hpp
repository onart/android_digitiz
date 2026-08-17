#pragma once

// Turns a stream of POINTER messages into injector calls.
//
// The guest is not trusted to send a well-formed sequence: packets can be lost
// on a reconnect, the app can be killed mid-stroke, and a buggy build can send
// nonsense. Every path here ends with the mouse button released, because the
// failure mode that actually hurts the user is a button stuck down on their PC.

#include <cstdint>

#include <digitiz/proto/messages.hpp>

#include "input/InputInjector.hpp"

namespace digitiz::host {

class PointerPipeline {
public:
    struct Stats {
        std::uint64_t received = 0;
        std::uint64_t injected = 0;
        std::uint64_t dropped_disabled = 0;
        std::uint64_t protocol_errors = 0; // duplicate DOWN, UP with no DOWN, ...
        std::uint64_t inject_failures = 0;
    };

    explicit PointerPipeline(IInputInjector& injector) : injector_(&injector) {}

    // Turning this off mid-stroke releases the held button first.
    void set_enabled(bool on);
    bool enabled() const noexcept { return enabled_; }

    void handle(const proto::Pointer& p);

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

    IInputInjector* injector_;
    bool enabled_ = false;
    bool down_ = false;
    proto::MouseButton held_ = proto::MouseButton::Left;
    std::int32_t last_x_ = 0;
    std::int32_t last_y_ = 0;
    Stats stats_;
};

} // namespace digitiz::host
