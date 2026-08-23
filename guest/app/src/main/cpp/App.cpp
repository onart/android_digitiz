#include "App.hpp"

#include <cmath>

#include <GLES3/gl3.h>
#include <android/configuration.h>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

std::uint64_t now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

App::App(android_app* app)
    : app_(app),
      router_(view_, [this](const proto::Pointer& p) { link_.send(proto::encode(p)); }) {

    // GameActivity gives native code the external data path directly, so the
    // settings file needs no JNI.
    settings_.load(app->activity != nullptr ? app->activity->externalDataPath : nullptr);
    menu_.set_auto_launch(settings_.auto_launch());

    router_.set_ui_hit_test([this](core::Vec2 p) { return menu_.hit_test(p); });
    router_.set_pc_point_test([this](core::Vec2 pc) { return pc_point_on_screen(pc); });

    link_.set_handlers(
        [this](proto::MsgType type, std::span<const std::byte> payload) {
            on_message(type, payload);
        },
        [this] { on_link_up(); }, [this] { on_link_down(); });
}

App::~App() {
    link_.stop();
    grid_.release();
    ui_.release();
}

void App::start_link() {
    link_.start(kHostPort);
}

void App::forward_log(core::LogLevel level, std::string_view text) {
    // Debug and trace stay on the device; the host console is for things the
    // user could plausibly need to see without logcat.
    if (level < core::LogLevel::Info) {
        return;
    }
    if (link_.state() != LinkState::Connected) {
        return;
    }

    // send() failing logs a warning, which would come straight back here.
    static thread_local bool forwarding = false;
    if (forwarding) {
        return;
    }
    forwarding = true;
    link_.send(proto::encode(proto::LogMessage{level, std::string(text)}));
    forwarding = false;
}

void App::on_command(std::int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (gl_.attach(app_->window)) {
            if (!renderers_ready_) {
                renderers_ready_ = grid_.init() && ui_.init();
                if (!renderers_ready_) {
                    DZ_ERROR("renderer initialisation failed");
                }
            }

            // Text is optional: without it the UI still works, just wordless.
            // The JNI attachment survives a surface bounce, only GL does not.
            if (!text_.attached()) {
                text_.init(app_->activity);
            } else {
                text_.init_gl();
            }
            menu_.load_labels(text_);
            if (app_->config != nullptr) {
                const int dpi = AConfiguration_getDensity(app_->config);
                if (dpi > 0) {
                    density_ = static_cast<float>(dpi) / 160.0f;
                }
            }
            menu_.layout(gl_.width(), gl_.height(), density_);
            // Only frame the view the first time. Coming back from the home
            // screen must not undo a pan and zoom the user set up.
            if (!view_fitted_) {
                fit_view_to_desktop();
            }
            start_link();
        }
        break;

    case APP_CMD_TERM_WINDOW:
        // GL objects die with the context, so they must be rebuilt on return.
        grid_.release();
        ui_.release();
        text_.release_gl();
        renderers_ready_ = false;
        gl_.detach();
        break;

    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED: {
        const int was_w = gl_.width();
        const int was_h = gl_.height();
        gl_.refresh_size();
        menu_.layout(gl_.width(), gl_.height(), density_);

        // A rotation changes how much of the desktop fits. Re-frame it, unless
        // the user has already pinched or panned — then the view is theirs.
        const bool resized = gl_.width() != was_w || gl_.height() != was_h;
        if (resized && !router_.view_adjusted_by_user()) {
            fit_view_to_desktop();
        }
        break;
    }

    case APP_CMD_GAINED_FOCUS:
        has_focus_ = true;
        break;

    case APP_CMD_LOST_FOCUS:
        has_focus_ = false;
        // A stroke cannot be finished if the app is not in front; releasing it
        // now is what keeps the PC's mouse button from sticking.
        router_.cancel_stroke();
        break;

    default:
        break;
    }
}

void App::frame() {
    if (!gl_.ready() || !renderers_ready_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    double dt = 1.0 / 60.0;
    if (last_frame_.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<double>(now - last_frame_).count();
    }
    last_frame_ = now;

    apply_pending();
    drain_input();

    // The menu owns the switch; the router owns the behaviour.
    if (menu_.take_mode_change()) {
        router_.set_mode(menu_.mode());
    }
    if (menu_.take_auto_launch_change()) {
        // Written straight to disk: the host reads this file while the app is
        // not running, so it has to be current before the app closes.
        settings_.set_auto_launch(menu_.auto_launch());
    }

    menu_.advance(dt);
    render();
    gl_.swap();
}

void App::drain_input() {
    android_input_buffer* input = android_app_swap_input_buffers(app_);
    if (input == nullptr) {
        return;
    }

    if (input->motionEventsCount > 0) {
        for (std::uint64_t i = 0; i < input->motionEventsCount; ++i) {
            router_.handle(input->motionEvents[i]);
        }
        android_app_clear_motion_events(input);
    }
    if (input->keyEventsCount > 0) {
        android_app_clear_key_events(input);
    }
}

void App::apply_pending() {
    bool fit = false;
    bool cancel = false;
    {
        std::lock_guard lock(pending_mutex_);
        if (view_needs_fit_) {
            view_needs_fit_ = false;
            fit = true;
        }
        if (stroke_cancel_pending_) {
            stroke_cancel_pending_ = false;
            cancel = true;
        }
    }
    if (cancel) {
        // The host already released on its side; this just stops us from
        // continuing a stroke it has no record of once we reconnect.
        router_.cancel_stroke();
    }
    if (fit) {
        fit_view_to_desktop();
    }
}

bool App::pc_point_on_screen(core::Vec2 pc) const {
    const auto x = static_cast<std::int32_t>(std::lround(pc.x));
    const auto y = static_cast<std::int32_t>(std::lround(pc.y));

    std::lock_guard lock(pending_mutex_);
    for (const core::Recti& m : monitors_) {
        if (m.contains(x, y)) {
            return true;
        }
    }
    return false;
}

void App::fit_view_to_desktop() {
    if (gl_.width() <= 0 || gl_.height() <= 0) {
        return;
    }
    core::Recti desktop;
    {
        // view_fitted_ is read by the network thread when deciding whether a
        // HELLO_ACK should re-frame, so it is set under the same lock.
        std::lock_guard lock(pending_mutex_);
        desktop = desktop_;
        view_fitted_ = true;
    }

    // Leave a margin so the desktop outline is visible rather than flush with
    // the screen edge.
    view_.fit(desktop, gl_.width(), gl_.height(), 0.12);
    DZ_INFO("view fitted to desktop %dx%d at (%d, %d), scale %.4f", desktop.w, desktop.h, desktop.x,
            desktop.y, view_.scale());
}

void App::render() {
    core::Recti desktop;
    std::vector<core::Recti> monitors;
    bool enabled = false;
    bool linked = false;
    {
        std::lock_guard lock(pending_mutex_);
        desktop = desktop_;
        monitors = monitors_;
        enabled = host_enabled_;
        linked = link_up_;
    }

    glViewport(0, 0, gl_.width(), gl_.height());
    grid_.draw(view_, gl_.width(), gl_.height(), monitors, enabled, linked);

    ui_.begin(gl_.width(), gl_.height());
    minimap_.draw(ui_, view_, gl_.width(), gl_.height(), desktop, monitors, density_);
    menu_.draw(ui_);
    ui_.end();

    // Separate pass: shapes and text use different programs, and interleaving
    // them would mean rebinding on every widget.
    if (text_.ready()) {
        text_.begin(gl_.width(), gl_.height());
        menu_.draw_labels(text_);
        text_.end();
    }
}

// ---------------------------------------------------------------------------
// Network thread

void App::on_link_up() {
    {
        std::lock_guard lock(pending_mutex_);
        link_up_ = true;
    }
    heartbeat_seen_ = false;
    send_hello();
}

void App::on_link_down() {
    std::lock_guard lock(pending_mutex_);
    link_up_ = false;
    host_enabled_ = false;
    stroke_cancel_pending_ = true;
}

void App::send_hello() {
    proto::Hello hello;
    hello.proto_ver = proto::kProtocolVersion;
    hello.screen_w = gl_.width();
    hello.screen_h = gl_.height();
    hello.density = density_;
    hello.device = "Digitiz guest";
    link_.send(proto::encode(hello));
    // The reply is the interesting event; this one would just repeat on every
    // retry while the host is down.
    DZ_DEBUG("sent HELLO: %d x %d density %.2f", hello.screen_w, hello.screen_h,
             static_cast<double>(hello.density));
}

void App::on_message(proto::MsgType type, std::span<const std::byte> payload) {
    switch (type) {
    case proto::MsgType::HelloAck: {
        proto::HelloAck ack;
        if (!proto::decode(payload, ack)) {
            DZ_WARN("malformed HELLO_ACK");
            return;
        }
        DZ_INFO("host: %s, virtual desktop %dx%d at (%d, %d), %zu monitor(s)",
                proto::to_string(ack.host_os), ack.vw, ack.vh, ack.vx, ack.vy,
                ack.monitors.size());

        const core::Recti fresh{ack.vx, ack.vy, ack.vw, ack.vh};

        std::vector<core::Recti> screens;
        screens.reserve(ack.monitors.size());
        for (const proto::Monitor& m : ack.monitors) {
            screens.push_back(core::Recti{m.x, m.y, m.w, m.h});
        }
        // A host that reports no monitors still has a desktop; treat the whole
        // bounding box as live rather than rejecting every touch.
        if (screens.empty()) {
            screens.push_back(fresh);
        }

        std::lock_guard lock(pending_mutex_);
        // Re-frame only when the desktop actually changed, so reconnecting to
        // the same PC leaves the user's view where they put it.
        if (!view_fitted_ || fresh != desktop_) {
            view_needs_fit_ = true;
        }
        desktop_ = fresh;
        monitors_ = std::move(screens);
        break;
    }

    case proto::MsgType::HostState: {
        proto::HostState state;
        if (!proto::decode(payload, state)) {
            return;
        }
        std::lock_guard lock(pending_mutex_);
        host_enabled_ = state.enabled;
        break;
    }

    case proto::MsgType::Ping: {
        proto::Ping ping;
        if (!proto::decode(payload, ping)) {
            DZ_WARN("malformed PING");
            return;
        }
        if (!link_.send(proto::encode(proto::Pong{ping.t_send_us, now_us()}))) {
            DZ_WARN("failed to answer PING");
        } else if (!heartbeat_seen_) {
            heartbeat_seen_ = true;
            DZ_INFO("heartbeat established");
        }
        break;
    }

    default:
        break;
    }
}

} // namespace digitiz::guest
