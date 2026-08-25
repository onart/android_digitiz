#include "App.hpp"

#include <algorithm>
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
    menu_.set_screen_enabled(settings_.screen_enabled());
    screen_enabled_ = settings_.screen_enabled();
    apply_throttle();

    strip_.set_store(&buttons_);
    strip_.set_orientation(settings_.strip_vertical() ? StripOrientation::Vertical
                                                      : StripOrientation::Horizontal);
    strip_.set_expanded(settings_.strip_expanded());
    strip_.set_slot_limit(settings_.strip_slots());
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
    screen_.release();
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
                // Not part of renderers_ready_: without it the app is exactly
                // what it was before this feature existed, which is a working
                // digitizer. Only the screen picture is missing.
                if (!screen_.init()) {
                    DZ_WARN("screen renderer unavailable; the PC picture is off");
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
            if (default_preset_name_.empty()) {
                // The preset that exists because one always has to carries no
                // name of its own; naming it down in the store would mean
                // inventing an English one there.
                default_preset_name_ = text_.localized("preset_default");
                refresh_preset_offer();
            }
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
        // Nothing will be drawn or uploaded until the surface comes back, so
        // the host is told to stop rather than left filling a queue nobody is
        // emptying.
        stop_stream();
        // GL objects die with the context, so they must be rebuilt on return.
        grid_.release();
        screen_.release();
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
    if (menu_.take_strip_slots_change()) {
        strip_.set_slot_limit(menu_.strip_slots());
        relayout_widgets();
        settings_.set_strip_slots(menu_.strip_slots());
    }
    if (strip_.take_add_request()) {
        open_button_editor(-1);
    }
    if (strip_.take_preset_menu_request()) {
        open_preset_menu();
    }
    if (strip_.take_suggestion_accepted()) {
        if (buttons_.select(offered_preset_)) {
            DZ_INFO("preset: switched to %s for %s",
                    preset_display_name(buttons_.current()).c_str(), focused_process_.c_str());
            relayout_widgets();
        }
        refresh_preset_offer();
    }
    if (strip_.take_suggestion_declined()) {
        // Remembered against the program, not the moment: the focus can come
        // back to it half a second later and being asked again would be worse
        // than not asking at all.
        offer_declined_for_ = focused_process_;
        refresh_preset_offer();
    }
    apply_preset_events();
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
    if (menu_.take_screen_change()) {
        screen_enabled_ = menu_.screen_enabled();
        settings_.set_screen_enabled(screen_enabled_);
    }
    update_stream();
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
        if (active_window != focused_process_) {
            // A different program, so an earlier refusal no longer applies.
            offer_declined_for_.clear();
        }
        focused_process_ = active_window;
        strip_.set_active_window(std::move(active_window));
        refresh_preset_offer();
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
    // The ceiling depends on the direction the strip is running and on the
    // screen, so only the strip can say what it is; the menu just shows it.
    menu_.set_strip_slots(strip_.slot_limit(), strip_.max_slots());
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

std::string App::preset_display_name(int index) const {
    if (!buttons_.valid_preset(index)) {
        return default_preset_name_;
    }
    const std::string& name = buttons_.presets()[static_cast<std::size_t>(index)].name;
    return name.empty() ? default_preset_name_ : name;
}

void App::refresh_preset_offer() {
    strip_.set_preset_name(preset_display_name(buttons_.current()));

    const int wanted = buttons_.preset_for(focused_process_);
    const bool worth_asking =
        wanted >= 0 && wanted != buttons_.current() && focused_process_ != offer_declined_for_;

    offered_preset_ = worth_asking ? wanted : -1;
    strip_.set_suggestion(worth_asking ? preset_display_name(wanted) : std::string());
}

void App::open_preset_menu() {
    std::vector<std::string> names;
    names.reserve(buttons_.presets().size());
    for (std::size_t i = 0; i < buttons_.presets().size(); ++i) {
        const Preset& preset = buttons_.presets()[i];
        std::string entry = preset_display_name(static_cast<int>(i));
        if (static_cast<int>(i) == buttons_.current()) {
            entry = "> " + entry;
        }
        if (!preset.match.empty()) {
            entry += "   (" + preset.match + ")";
        }
        names.push_back(std::move(entry));
    }
    show_preset_menu(app_->activity, names, buttons_.current(), focused_process_);
}

void App::apply_preset_events() {
    std::vector<PresetCommand> commands;
    drain_preset_events(commands);
    if (commands.empty()) {
        return;
    }

    for (const PresetCommand& c : commands) {
        switch (c.kind) {
        case PresetCommandKind::Select:
            buttons_.select(c.index);
            break;
        case PresetCommandKind::Create:
            buttons_.create(c.text);
            break;
        case PresetCommandKind::Rename:
            buttons_.rename(c.index, c.text);
            break;
        case PresetCommandKind::Bind:
            // Bound to the program that was in focus when the menu opened,
            // which is the one the user was looking at when they decided.
            buttons_.set_match(c.index, c.text);
            break;
        case PresetCommandKind::Unbind:
            buttons_.set_match(c.index, std::string());
            break;
        case PresetCommandKind::Delete:
            buttons_.remove_preset(c.index);
            break;
        }
    }

    // A different preset means a different number of buttons, and binding
    // changes what should be offered.
    relayout_widgets();
    refresh_preset_offer();
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

// ---------------------------------------------------------------------------
// Screen transfer

namespace {

// How long the view has to hold still before a new region is asked for.
//
// Every change makes the host renumber its tiles and treat all of them as
// unsent, so asking during a pinch would keep restarting the transfer and
// never finish one. Same reasoning, and the same order of magnitude, as the
// host's wait before it believes a window really has the focus.
constexpr double kSettleSeconds = 0.25;

// The region is snapped outwards to this many PC pixels, and the encoded size
// to a multiple of the tile. Both stop a slow drag from redefining the request
// on every frame -- the second more firmly, because a change in encoded size
// also throws away the texture.
constexpr int kRegionSnap = 32;
constexpr int kSizeSnap = 64;
constexpr int kMaxEncoded = 2048;

int floor_to(double v, int step) {
    return static_cast<int>(std::floor(v / step)) * step;
}

int ceil_to(double v, int step) {
    return static_cast<int>(std::ceil(v / step)) * step;
}

bool same_request(const proto::ViewportReq& a, const proto::ViewportReq& b) {
    // Exact, because both sides of the comparison are already snapped: a
    // tolerance here would only be a second, softer snap.
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && a.out_w == b.out_w &&
           a.out_h == b.out_h && a.fps == b.fps && a.format == b.format && a.tile == b.tile &&
           a.flags == b.flags;
}

} // namespace

// A default-constructed request is the "send nothing" one: format Off, zero
// fps. So every empty return below is the same answer -- there is nothing here
// worth asking for.
proto::ViewportReq App::wanted_viewport(core::Recti desktop) const {
    proto::ViewportReq req;
    if (desktop.w <= 0 || desktop.h <= 0 || gl_.width() <= 0 || gl_.height() <= 0) {
        return req;
    }

    const core::Vec2 top_left = view_.to_pc(core::Vec2{0.0, 0.0});
    const core::Vec2 bottom_right = view_.to_pc(
        core::Vec2{static_cast<double>(gl_.width()), static_cast<double>(gl_.height())});

    const int x0 = std::max(floor_to(top_left.x, kRegionSnap), desktop.x);
    const int y0 = std::max(floor_to(top_left.y, kRegionSnap), desktop.y);
    const int x1 = std::min(ceil_to(bottom_right.x, kRegionSnap), desktop.x + desktop.w);
    const int y1 = std::min(ceil_to(bottom_right.y, kRegionSnap), desktop.y + desktop.h);
    if (x1 <= x0 || y1 <= y0) {
        // The view is off the side of the desktop entirely.
        return req;
    }

    req.x = x0;
    req.y = y0;
    req.w = x1 - x0;
    req.h = y1 - y0;

    // Never ask for more pixels than the region has: past 1:1 the host would
    // be spending bandwidth on an upscale this side does for free.
    const double ratio = std::min(view_.scale(), 1.0);
    const auto encoded = [&](int span) {
        const int want = static_cast<int>(std::lround(span * ratio));
        const int snapped = ((want + kSizeSnap / 2) / kSizeSnap) * kSizeSnap;
        return std::clamp(std::max(snapped, kSizeSnap), 4, std::min(span, kMaxEncoded));
    };
    req.out_w = static_cast<std::uint16_t>(encoded(req.w));
    req.out_h = static_cast<std::uint16_t>(encoded(req.h));

    req.fps = 30;
    req.format = proto::FrameFormat::Etc2Rgb8;
    req.tile = 64;
    return req;
}

void App::stop_stream() {
    // Say so, rather than just stopping to listen. Without this the host goes
    // on encoding and sending into a queue nobody is emptying -- which is
    // exactly what backgrounding the app did, and it showed up as dropped
    // batches on the way back in.
    if (sent_viewport_.fps > 0 && link_.state() == LinkState::Connected) {
        link_.send(proto::encode(proto::ViewportReq{}));
    }
    frames_.reset();
    screen_.clear();
    sent_viewport_ = proto::ViewportReq{};
    wanted_viewport_ = proto::ViewportReq{};
    image_logged_ = false;
}

void App::update_stream() {
    core::Recti desktop;
    bool linked = false;
    {
        std::lock_guard lock(pending_mutex_);
        desktop = desktop_;
        linked = link_up_;
    }

    if (!linked) {
        // A frozen desktop is worse than none: it looks current and is not.
        // Nothing goes out -- there is nobody to send it to, and the next host
        // starts knowing nothing anyway, which is what an empty request says.
        if (sent_viewport_.fps > 0 || screen_.has_image()) {
            stop_stream();
        }
        return;
    }

    // Slide mode has no fixed place on the PC, so there is no region to ask
    // for -- the same reason the screen outlines and the minimap go away.
    const bool absolute = router_.mode() != InputMode::Slide;
    const proto::ViewportReq want = (screen_enabled_ && absolute && screen_.ready())
                                        ? wanted_viewport(desktop)
                                        : proto::ViewportReq{};

    if (want.fps == 0) {
        // Stopping does not wait for anything to settle. It is what the user
        // just asked for, and every frame it is late is bandwidth the pointer
        // wanted.
        if (sent_viewport_.fps > 0 || screen_.has_image()) {
            stop_stream();
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!same_request(want, wanted_viewport_)) {
        wanted_viewport_ = want;
        wanted_since_ = now;
        return;
    }
    if (same_request(want, sent_viewport_)) {
        return;
    }
    if (std::chrono::duration<double>(now - wanted_since_).count() < kSettleSeconds) {
        return;
    }

    if (!link_.send(proto::encode(want))) {
        return; // the link reports its own failures; try again next frame
    }
    sent_viewport_ = want;
    DZ_INFO("screen: asked for %dx%d at (%d, %d) as %ux%u", want.w, want.h, want.x, want.y,
            want.out_w, want.out_h);
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

    // Uploaded here rather than where they arrive: this is the thread with a
    // GL context, and the blocks reach the GPU untouched.
    frames_.take(batches_);
    for (const FrameBatch& batch : batches_) {
        screen_.upload(batch);
    }
    if (!batches_.empty() && !image_logged_ && screen_.has_image()) {
        image_logged_ = true;
        DZ_INFO("screen: first picture from the PC");
    }
    if (const FrameReceiver::Stats s = frames_.stats();
        s.malformed + s.dropped > bad_batches_logged_) {
        bad_batches_logged_ = s.malformed + s.dropped;
        DZ_WARN("screen: %llu malformed, %llu dropped, %llu stray chunk(s)",
                static_cast<unsigned long long>(s.malformed),
                static_cast<unsigned long long>(s.dropped),
                static_cast<unsigned long long>(s.stray_seq));
    }

    glViewport(0, 0, gl_.width(), gl_.height());
    grid_.draw(view_, gl_.width(), gl_.height(), shown_screens, enabled, linked);

    // Over the grid, not under it. The grid is the background, and where there
    // is a picture of the PC that picture is what "this is the PC" means; the
    // grid goes on saying it everywhere the picture does not reach.
    const bool showing_screen = absolute && screen_.has_image();
    if (showing_screen) {
        screen_.draw(view_, gl_.width(), gl_.height());
    }

    ui_.begin(gl_.width(), gl_.height());
    // The minimap answers "which part of the PC is under my finger?". With the
    // screen on, the screen answers it better.
    if (absolute && !showing_screen) {
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
    // Whatever was half assembled describes a session that no longer exists.
    // The picture itself is dropped on the render thread, which owns it.
    frames_.reset();

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

    case proto::MsgType::FrameInfo:
    case proto::MsgType::FrameData:
        // Straight through: assembling a batch is its own job, and the render
        // thread picks the result up when it has a GL context to put it in.
        frames_.on_message(type, payload);
        break;

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
