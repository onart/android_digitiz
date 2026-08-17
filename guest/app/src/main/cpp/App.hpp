#pragma once

// Guest application: owns the GL surface, the link to the host, the view
// transform, and the frame loop.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <digitiz/core/geometry.hpp>
#include <digitiz/proto/messages.hpp>

#include "input/TouchRouter.hpp"
#include "net/TcpTransport.hpp"
#include "render/GlContext.hpp"
#include "render/GridRenderer.hpp"
#include "render/UiRenderer.hpp"
#include "ui/Minimap.hpp"
#include "ui/SideMenu.hpp"

namespace digitiz::guest {

// Fixed on the device side because `adb reverse` maps this port; the host end
// is ephemeral. Must match kDevicePort in the host's Transport.hpp.
inline constexpr std::uint16_t kHostPort = 27183;

class App {
public:
    explicit App(android_app* app);
    ~App();

    void on_command(std::int32_t cmd);
    void frame();

    bool has_focus() const noexcept { return has_focus_; }

    // Mirrors a log line to the host console so both sides share one timeline.
    // Safe to call from any thread, including from inside the log sink itself.
    void forward_log(core::LogLevel level, std::string_view text);

    // True when there is a surface to draw into. Focus is deliberately not
    // part of this: a system dialog on top should not stop the frame loop,
    // only stop input from reaching us.
    bool wants_frames() const noexcept { return gl_.ready() && renderers_ready_; }

private:
    void start_link();
    void drain_input();
    void apply_pending();
    void render();

    // Runs on the network thread.
    void on_message(proto::MsgType type, std::span<const std::byte> payload);
    void on_link_up();
    void on_link_down();

    void send_hello();
    void fit_view_to_desktop();
    bool pc_point_on_screen(core::Vec2 pc) const;

    android_app* app_;
    GlContext gl_;
    GridRenderer grid_;
    UiRenderer ui_;
    SideMenu menu_;
    Minimap minimap_;
    TcpTransport link_;

    core::ViewTransform view_;
    TouchRouter router_;

    bool has_focus_ = false;
    bool renderers_ready_ = false;
    float density_ = 1.0f;
    std::chrono::steady_clock::time_point last_frame_{};

    // Written by the network thread, applied on the render thread.
    mutable std::mutex pending_mutex_;
    core::Recti desktop_{0, 0, 1920, 1080};
    // The screens themselves. Input is accepted on their union, which is not
    // the same as `desktop_` once monitors differ in size or alignment.
    std::vector<core::Recti> monitors_{core::Recti{0, 0, 1920, 1080}};
    bool host_enabled_ = false;
    bool link_up_ = false;
    bool view_needs_fit_ = false;
    bool heartbeat_seen_ = false;
    // The link dropping has to cancel any stroke, but that must happen on the
    // render thread — TouchRouter belongs to it.
    bool stroke_cancel_pending_ = false;

    // Set once the view has been framed. Backgrounding and returning must not
    // throw away a pan and zoom the user set up by hand.
    bool view_fitted_ = false;
};

} // namespace digitiz::guest
