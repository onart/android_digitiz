#pragma once

// Guest application: owns the GL surface, the link to the host, the view
// transform, and the frame loop.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <digitiz/core/geometry.hpp>
#include <digitiz/proto/messages.hpp>

#include "buttons/ButtonStore.hpp"
#include "core/Settings.hpp"
#include "input/TouchRouter.hpp"
#include "net/TcpTransport.hpp"
#include "render/GlContext.hpp"
#include "render/GridRenderer.hpp"
#include "render/UiRenderer.hpp"
#include "text/TextRenderer.hpp"
#include "ui/ButtonStrip.hpp"
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
    void relayout_widgets();
    // Everything the Java dialogs produced since the last frame.
    void apply_button_events();
    void apply_preset_events();
    void open_button_editor(int index);
    void open_preset_menu();
    // Recomputes what, if anything, to offer for the program now in focus, and
    // refreshes the caption either way.
    void refresh_preset_offer();
    std::string preset_display_name(int index) const;
    void send_button_pointer(proto::PointerAction action, core::Vec2 pc);
    void send_button_shortcut(const CustomButton& button);
    bool pc_point_on_screen(core::Vec2 pc) const;
    void apply_throttle();

    android_app* app_;
    GlContext gl_;
    GridRenderer grid_;
    UiRenderer ui_;
    TextRenderer text_;
    SideMenu menu_;
    ButtonStore buttons_;
    ButtonStrip strip_;
    Minimap minimap_;
    TcpTransport link_;
    Settings settings_;

    core::ViewTransform view_;
    TouchRouter router_;

    // Which widget owns the finger. The router hands every event of a gesture
    // to whoever claimed the press, so this only has to survive between them.
    enum class UiOwner : std::uint8_t { None, Menu, Strip };
    UiOwner ui_owner_ = UiOwner::None;

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
    // What the PC is focused on. Milestone 2 keys button presets off this;
    // until those exist it is shown in the drawer, which is enough to tell
    // whether the host is reporting what it should.
    std::string active_process_;
    bool active_window_dirty_ = false;
    bool link_up_ = false;
    bool view_needs_fit_ = false;
    bool heartbeat_seen_ = false;
    // The link dropping has to cancel any stroke, but that must happen on the
    // render thread — TouchRouter belongs to it.
    bool stroke_cancel_pending_ = false;

    // Render-thread copies of what the network thread reported, kept because
    // the preset offer has to be recomputed whenever either side changes.
    std::string focused_process_;
    // The program the user has already said no to. Cleared when the focus
    // moves elsewhere, so the offer comes back if they return to it later.
    std::string offer_declined_for_;
    int offered_preset_ = -1;
    std::string default_preset_name_;

    // Set once the view has been framed. Backgrounding and returning must not
    // throw away a pan and zoom the user set up by hand.
    bool view_fitted_ = false;
};

} // namespace digitiz::guest
