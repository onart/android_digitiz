#include "App.hpp"

#include <cmath>

#include <GLES3/gl3.h>
#include <android/configuration.h>

#include <digitiz/core/log.hpp>

#include "platform/ActivityBridge.hpp"

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
    const char* external = app->activity != nullptr ? app->activity->externalDataPath : nullptr;
    settings_.load(external);
    buttons_.load(external);

    menu_.set_auto_launch(settings_.auto_launch());
    menu_.set_throttle(settings_.min_interval_ms(), settings_.min_distance_dp());
    menu_.set_strip_vertical(settings_.strip_vertical());
    apply_throttle();

    strip_.set_buttons(&buttons_.buttons());
    strip_.set_orientation(settings_.strip_vertical() ? StripOrientation::Vertical
                                                      : StripOrientation::Horizontal);
    strip_.set_expanded(settings_.strip_expanded());
    strip_.set_sinks(
        [this](proto::PointerAction action, core::Vec2 pc) { send_button_pointer(action, pc); },
        [this](const CustomButton& b) { send_button_shortcut(b); });

    // The drawer covers the screen when it is open, so it gets first refusal
    // then. Closed, its handle and the strip cannot overlap, and the order
    // between them stops mattering.
    router_.set_ui_handlers(
        [this](core::Vec2 p) {
            if (!menu_.open() && strip_.hit_test(p)) {
                ui_owner_ = UiOwner::Strip;
                return true;
            }
            if (menu_.hit_test(p)) {
                ui_owner_ = UiOwner::Menu;
                return true;
            }
            ui_owner_ = UiOwner::None;
            return false;
        },
        [this](core::Vec2 p) {
            if (ui_owner_ == UiOwner::Strip) {
                strip_.drag(p);
            } else if (ui_owner_ == UiOwner::Menu) {
                menu_.drag(p);
            }
        },
        [this](core::Vec2 p) {
            if (ui_owner_ == UiOwner::Strip) {
                strip_.release(p);
            } else if (ui_owner_ == UiOwner::Menu) {
                menu_.release(p);
            }
            ui_owner_ = UiOwner::None;
        });
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
            strip_.load_labels(text_);
            if (app_->config != nullptr) {
                const int dpi = AConfiguration_getDensity(app_->config);
                if (dpi > 0) {
                    density_ = static_cast<float>(dpi) / 160.0f;
                    router_.set_density(density_);
                    // The distance threshold is stored in dp, so it cannot be
                    // converted until the real density is known.
                    apply_throttle();
                }
            }
            relayout_widgets();
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
        relayout_widgets();

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
        // Same for a widget holding the finger. This is not hypothetical: the
        // long-press menu is a dialog, so opening it is itself a focus loss,
        // and the finger that opened it never reports going up.
        if (router_.ui_captured()) {
            router_.cancel_ui_capture();
            if (ui_owner_ == UiOwner::Strip) {
                strip_.cancel_press();
            } else if (ui_owner_ == UiOwner::Menu) {
                menu_.cancel_press();
            }
            ui_owner_ = UiOwner::None;
        }
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
    if (menu_.take_strip_change()) {
        strip_.set_orientation(menu_.strip_vertical() ? StripOrientation::Vertical
                                                      : StripOrientation::Horizontal);
        relayout_widgets();
        settings_.set_strip(menu_.strip_vertical(), strip_.expanded());
    }
    if (strip_.take_state_change()) {
        settings_.set_strip(menu_.strip_vertical(), strip_.expanded());
    }
    if (strip_.take_add_request()) {
        open_button_editor(-1);
    }
    if (const int held = strip_.take_long_press(); buttons_.valid(held)) {
        show_button_menu(app_->activity, held,
                         buttons_.buttons()[static_cast<std::size_t>(held)].label);
    }
    apply_button_events();
    if (menu_.take_rotate_request()) {
        // The activity owns this, not us: rotating what we draw would leave
        // the system bars and the touch mapping on the old side.
        flip_orientation(app_->activity);
    }
    if (menu_.take_auto_launch_change()) {
        // Written straight to disk: the host reads this file while the app is
        // not running, so it has to be current before the app closes.
        settings_.set_auto_launch(menu_.auto_launch());
    }
    if (menu_.take_throttle_change()) {
        settings_.set_throttle(menu_.min_interval_ms(), menu_.min_distance_dp());
        apply_throttle();
    }

    menu_.advance(dt);
    strip_.advance(dt);
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
    bool have_active_window = false;
    std::string active_window;
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
        if (active_window_dirty_) {
            active_window_dirty_ = false;
            active_window = active_process_;
            have_active_window = true;
        }
    }
    if (have_active_window) {
        // Beside the buttons rather than in the drawer: it is the answer to
        // which preset is up, and an answer you have to open a drawer to read
        // cannot do that job.
        strip_.set_active_window(std::move(active_window));
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

void App::relayout_widgets() {
    menu_.layout(gl_.width(), gl_.height(), density_);
    strip_.layout(gl_.width(), gl_.height(), density_);
}

void App::apply_button_events() {
    std::vector<ButtonEdit> edits;
    std::vector<ButtonCommand> commands;
    drain_button_events(edits, commands);
    if (edits.empty() && commands.empty()) {
        return;
    }

    for (const ButtonEdit& e : edits) {
        CustomButton button;
        button.kind = static_cast<ButtonKind>(e.kind);
        button.label = e.label;
        button.target = core::Recti{e.x, e.y, e.w, e.h};
        button.modifiers = static_cast<std::uint8_t>(e.modifiers);
        button.key = e.key;

        if (e.index < 0) {
            buttons_.add(std::move(button));
        } else {
            buttons_.replace(e.index, std::move(button));
        }
    }

    for (const ButtonCommand& c : commands) {
        switch (c.kind) {
        case ButtonCommandKind::Edit:
            open_button_editor(c.index);
            break;
        case ButtonCommandKind::Delete:
            buttons_.remove(c.index);
            break;
        case ButtonCommandKind::Earlier:
            buttons_.move(c.index, -1);
            break;
        case ButtonCommandKind::Later:
            buttons_.move(c.index, 1);
            break;
        }
    }

    // How many buttons there are decides how many slots fit and whether the
    // cycling arrows have to be paid for, so the run is measured again.
    relayout_widgets();
}

void App::open_button_editor(int index) {
    if (!buttons_.valid(index)) {
        show_button_editor(app_->activity, -1, 0, "", 0, 0, 0, 0, 0, "");
        return;
    }
    const CustomButton& b = buttons_.buttons()[static_cast<std::size_t>(index)];
    show_button_editor(app_->activity, index, static_cast<int>(b.kind), b.label, b.target.x,
                       b.target.y, b.target.w, b.target.h, b.modifiers, b.key);
}

void App::send_button_pointer(proto::PointerAction action, core::Vec2 pc) {
    proto::Pointer p;
    p.t_us = now_us();
    p.x = static_cast<std::int32_t>(std::lround(pc.x));
    p.y = static_cast<std::int32_t>(std::lround(pc.y));
    p.action = action;
    p.button = proto::MouseButton::Left;
    // Never the surface-relative mode: a button names a place, and that
    // meaning does not change because the drawing surface has been switched to
    // relative. It is window-relative instead, so the host resolves it against
    // whatever has the focus when it arrives.
    p.flags = proto::kPointerWindowRelative;
    link_.send(proto::encode(p));
}

void App::send_button_shortcut(const CustomButton& button) {
    proto::Key key;
    key.modifiers = button.modifiers;
    key.action = proto::KeyAction::Press;
    key.key = button.key;
    link_.send(proto::encode(key));
}

void App::apply_throttle() {
    router_.set_min_interval_us(static_cast<std::uint64_t>(settings_.min_interval_ms()) * 1000ull);
    // Stored in dp so the setting means the same thing on any screen; the
    // router compares raw surface pixels.
    router_.set_min_distance_px(static_cast<double>(settings_.min_distance_dp()) *
                                static_cast<double>(density_));
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

    // Slide mode has no fixed mapping, so the screen outlines and the minimap
    // would be pointing at nothing. Leaving them up would be worse than
    // leaving them out.
    const bool absolute = router_.mode() != InputMode::Slide;
    const std::span<const core::Recti> shown_screens =
        absolute ? std::span<const core::Recti>(monitors) : std::span<const core::Recti>();

    glViewport(0, 0, gl_.width(), gl_.height());
    grid_.draw(view_, gl_.width(), gl_.height(), shown_screens, enabled, linked);

    ui_.begin(gl_.width(), gl_.height());
    if (absolute) {
        // A vertical strip sits in the corner the minimap wants, so the map
        // starts past it rather than under it.
        float inset = 0.0f;
        if (strip_.orientation() == StripOrientation::Vertical) {
            const Rect occupied = strip_.occupied();
            inset = occupied.x + occupied.w;
        }
        minimap_.draw(ui_, view_, gl_.width(), gl_.height(), desktop, monitors, density_, inset);
    }
    strip_.draw(ui_);
    menu_.draw(ui_);
    ui_.end();

    // Separate pass: shapes and text use different programs, and interleaving
    // them would mean rebinding on every widget.
    if (text_.ready()) {
        text_.begin(gl_.width(), gl_.height());
        strip_.draw_labels(text_);
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
    // Nothing is focused on a PC we are not talking to. Leaving the last name
    // up would have a preset still claiming to follow a program we can no
    // longer see.
    active_process_.clear();
    active_window_dirty_ = true;
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

    case proto::MsgType::ActiveWindow: {
        proto::ActiveWindow window;
        if (!proto::decode(payload, window)) {
            DZ_WARN("malformed ACTIVE_WINDOW");
            return;
        }
        // Debug, not info: this is the host telling us something it already
        // logged, and echoing it back would print every focus change twice in
        // the host console.
        DZ_DEBUG("PC active window: %s (pid %u)",
                 window.process.empty() ? "<unidentified>" : window.process.c_str(), window.pid);

        std::lock_guard lock(pending_mutex_);
        active_process_ = std::move(window.process);
        active_window_dirty_ = true;
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
