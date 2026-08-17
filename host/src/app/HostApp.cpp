#include "app/HostApp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include <imgui.h>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

namespace {

constexpr auto kLayoutRefreshInterval = std::chrono::seconds(1);

// SendInput lands in the input queue; the cursor position the OS reports lags
// it slightly. Long enough to settle, short enough that a 25-point sweep stays
// under a second.
constexpr auto kCursorSettleDelay = std::chrono::milliseconds(6);

constexpr auto kPingInterval = std::chrono::seconds(1);
constexpr int kMaxUnansweredPings = 3;

std::uint64_t now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

ImVec4 level_color(core::LogLevel level) {
    switch (level) {
    case core::LogLevel::Trace:
        return ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
    case core::LogLevel::Debug:
        return ImVec4(0.70f, 0.72f, 0.78f, 1.0f);
    case core::LogLevel::Info:
        return ImVec4(0.85f, 0.88f, 0.92f, 1.0f);
    case core::LogLevel::Warn:
        return ImVec4(1.00f, 0.78f, 0.35f, 1.0f);
    case core::LogLevel::Error:
        return ImVec4(1.00f, 0.45f, 0.45f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

} // namespace

HostApp::HostApp(LogStore& log) : log_(&log) {}

bool HostApp::init() {
    display_ = make_display_info();
    injector_ = make_input_injector();

    if (!display_ || !injector_) {
        DZ_ERROR("no display/input backend for this platform yet");
        return false;
    }

    pipeline_ = std::make_unique<PointerPipeline>(*injector_);

    refresh_layout(true);
    DZ_INFO("host ready");
    return true;
}

bool HostApp::start_transport() {
    transport_ = std::make_unique<AdbTransport>();
    transport_->set_handlers(
        [this](proto::MsgType type, std::span<const std::byte> payload) {
            on_transport_message(type, payload);
        },
        [this] { on_transport_connect(); }, [this] { on_transport_disconnect(); });
    return transport_->start();
}

void HostApp::tick() {
    refresh_layout(false);
    pump_session();
}

void HostApp::shutdown() {
    // Stop the transport first so nothing arrives while we are tearing down.
    if (transport_) {
        transport_->stop();
    }
    if (pipeline_) {
        // Whatever else happens, do not leave a mouse button held on the
        // user's desktop.
        std::lock_guard lock(pipeline_mutex_);
        pipeline_->end_session();
    }
}

// ---------------------------------------------------------------------------
// Transport thread

void HostApp::on_transport_connect() {
    {
        std::lock_guard lock(session_mutex_);
        guest_known_ = false;
        hello_ack_pending_ = false;
        rtt_ms_ = -1.0;
        unanswered_pings_ = 0;
        // The phone may have rebooted between sessions, so nothing measured
        // about its clock carries over.
        clock_.reset();
        link_latency_.reset();
        inject_latency_.reset();
    }
    session_active_ = true;
    last_ping_ = std::chrono::steady_clock::now();
}

void HostApp::on_transport_disconnect() {
    session_active_ = false;
    {
        // A dropped cable mid-stroke must not leave the button held.
        std::lock_guard lock(pipeline_mutex_);
        pipeline_->end_session();
    }
    std::lock_guard lock(session_mutex_);
    guest_known_ = false;
    rtt_ms_ = -1.0;
}

void HostApp::on_transport_message(proto::MsgType type, std::span<const std::byte> payload) {
    switch (type) {
    case proto::MsgType::Pointer: {
        proto::Pointer p;
        if (!proto::decode(payload, p)) {
            DZ_WARN("malformed POINTER payload (%zu bytes)", payload.size());
            return;
        }
        const std::uint64_t arrived_us = now_us();

        // Stroke boundaries are logged; MOVE is not, or a single drag would
        // bury everything else. The pipeline counters cover the volume.
        if (p.action != proto::PointerAction::Move) {
            DZ_DEBUG("POINTER %s at (%d, %d)", proto::to_string(p.action), p.x, p.y);
        }

        // Glass to here: the guest stamps t_us when Android delivered the
        // touch, so this covers the phone's input pipeline plus the tunnel.
        {
            std::lock_guard lock(session_mutex_);
            if (clock_.ready()) {
                const double link_ms =
                    (static_cast<double>(arrived_us) - clock_.to_host_us(p.t_us)) / 1000.0;
                // A negative or absurd figure means the clock estimate is
                // stale, not that the event arrived early.
                if (link_ms >= 0.0 && link_ms < 1000.0) {
                    link_latency_.add(link_ms);
                }
            }
        }

        const std::uint64_t before_inject = now_us();
        {
            std::lock_guard lock(pipeline_mutex_);
            pipeline_->handle(p);
        }
        {
            std::lock_guard lock(session_mutex_);
            inject_latency_.add(static_cast<double>(now_us() - before_inject) / 1000.0);
        }

        // One summary per stroke: fine-grained enough to correlate with how a
        // particular drag felt, quiet enough to leave on.
        if (p.action == proto::PointerAction::Up || p.action == proto::PointerAction::Cancel) {
            std::lock_guard lock(session_mutex_);
            if (!link_latency_.empty()) {
                DZ_DEBUG("stroke ended: event-to-host %.1f ms avg, %.1f max (%zu samples); "
                         "injection %.0f us avg",
                         link_latency_.average(), link_latency_.max(), link_latency_.size(),
                         inject_latency_.average() * 1000.0);
            }
        }
        break;
    }

    case proto::MsgType::Hello: {
        proto::Hello hello;
        if (!proto::decode(payload, hello)) {
            DZ_WARN("malformed HELLO payload (%zu bytes)", payload.size());
            return;
        }
        DZ_INFO("guest HELLO: \"%s\" %dx%d density %.2f (protocol v%u)", hello.device.c_str(),
                hello.screen_w, hello.screen_h, static_cast<double>(hello.density),
                static_cast<unsigned>(hello.proto_ver));

        if (hello.proto_ver != proto::kProtocolVersion) {
            DZ_ERROR("protocol mismatch: guest speaks v%u, host speaks v%u — dropping session",
                     static_cast<unsigned>(hello.proto_ver),
                     static_cast<unsigned>(proto::kProtocolVersion));
            transport_->drop_session();
            return;
        }

        std::lock_guard lock(session_mutex_);
        guest_ = std::move(hello);
        guest_known_ = true;
        // The reply needs the display layout, which the UI thread owns, so
        // hand it off rather than reaching across.
        hello_ack_pending_ = true;
        break;
    }

    case proto::MsgType::Pong: {
        proto::Pong pong;
        if (!proto::decode(payload, pong)) {
            return;
        }
        const std::uint64_t recv_us = now_us();
        const double rtt = static_cast<double>(recv_us - pong.t_send_us) / 1000.0;
        bool first = false;
        {
            std::lock_guard lock(session_mutex_);
            first = rtt_ms_ < 0.0;
            rtt_ms_ = rtt;
            unanswered_pings_ = 0;
            clock_.observe(pong.t_send_us, pong.t_reply_us, recv_us);
        }
        if (first) {
            DZ_INFO("heartbeat established, RTT %.2f ms", rtt);
        }
        break;
    }

    case proto::MsgType::Ping: {
        proto::Ping ping;
        if (!proto::decode(payload, ping)) {
            return;
        }
        const auto reply = proto::encode(proto::Pong{ping.t_send_us, now_us()});
        transport_->send(reply);
        break;
    }

    case proto::MsgType::Log: {
        proto::LogMessage msg;
        if (!proto::decode(payload, msg)) {
            return;
        }
        // Guest lines land in the same console as ours, on one timeline.
        core::log_write(msg.level, "[guest] " + msg.text);
        break;
    }

    default:
        DZ_DEBUG("ignoring %s (%zu byte payload)", proto::to_string(type), payload.size());
        break;
    }
}

// ---------------------------------------------------------------------------
// UI thread

void HostApp::pump_session() {
    if (!transport_ || !session_active_) {
        return;
    }

    bool send_ack = false;
    {
        std::lock_guard lock(session_mutex_);
        if (hello_ack_pending_) {
            hello_ack_pending_ = false;
            send_ack = true;
        }
    }
    if (send_ack) {
        send_hello_ack();
        send_host_state();
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_ping_ < kPingInterval) {
        return;
    }
    last_ping_ = now;

    int unanswered = 0;
    {
        std::lock_guard lock(session_mutex_);
        unanswered = ++unanswered_pings_;
    }

    if (unanswered > kMaxUnansweredPings) {
        DZ_WARN("guest missed %d heartbeats; dropping the session", unanswered - 1);
        transport_->drop_session();
        return;
    }

    // A healthy heartbeat is silent; the RTT is on screen. Only say something
    // once a reply has actually gone missing.
    if (unanswered > 1) {
        DZ_DEBUG("heartbeat: %d ping(s) unanswered", unanswered - 1);
    }
    transport_->send(proto::encode(proto::Ping{now_us()}));
}

void HostApp::send_hello_ack() {
    proto::HelloAck ack;
    ack.proto_ver = proto::kProtocolVersion;
#ifdef _WIN32
    ack.host_os = proto::HostOs::Windows;
#elif defined(__APPLE__)
    ack.host_os = proto::HostOs::MacOS;
#else
    ack.host_os = proto::HostOs::Linux;
#endif
    ack.vx = layout_.virtual_bounds.x;
    ack.vy = layout_.virtual_bounds.y;
    ack.vw = layout_.virtual_bounds.w;
    ack.vh = layout_.virtual_bounds.h;

    for (const MonitorInfo& m : layout_.monitors) {
        ack.monitors.push_back(proto::Monitor{m.bounds.x, m.bounds.y, m.bounds.w, m.bounds.h,
                                              m.dpi, m.primary});
    }

    if (transport_->send(proto::encode(ack))) {
        DZ_INFO("sent HELLO_ACK: virtual %dx%d at (%d, %d), %zu monitor(s)", ack.vw, ack.vh, ack.vx,
                ack.vy, ack.monitors.size());
    }
}

void HostApp::send_host_state() {
    proto::HostState state;
    state.enabled = enabled_;
    {
        std::lock_guard lock(pipeline_mutex_);
        state.injecting = pipeline_->stroke_active();
    }
    transport_->send(proto::encode(state));
}

void HostApp::refresh_layout(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_layout_query_ < kLayoutRefreshInterval) {
        return;
    }
    last_layout_query_ = now;

    DisplayLayout fresh = display_->query();
    if (!force && fresh == layout_) {
        return;
    }

    layout_ = std::move(fresh);
    injector_->set_virtual_bounds(layout_.virtual_bounds);

    const core::Recti& v = layout_.virtual_bounds;
    DZ_INFO("display layout: virtual %dx%d at (%d, %d), %zu monitor(s)", v.w, v.h, v.x, v.y,
            layout_.monitors.size());
    for (const MonitorInfo& m : layout_.monitors) {
        DZ_DEBUG("  %s%s %dx%d at (%d, %d), %u dpi", m.name.c_str(), m.primary ? " [primary]" : "",
                 m.bounds.w, m.bounds.h, m.bounds.x, m.bounds.y, m.dpi);
    }
}

core::Recti HostApp::primary_bounds() const {
    for (const MonitorInfo& m : layout_.monitors) {
        if (m.primary) {
            return m.bounds;
        }
    }
    return layout_.virtual_bounds;
}

// ---------------------------------------------------------------------------
// Self-test. The point of these is that they work with no transport and no
// phone: if a click misfires later, this tells you instantly whether injection
// or the network is at fault.

void HostApp::move_cursor_to_center() {
    const core::Recti b = primary_bounds();
    const std::int32_t cx = b.x + b.w / 2;
    const std::int32_t cy = b.y + b.h / 2;

    DZ_INFO("selftest: moving cursor to primary center (%d, %d)", cx, cy);
    injector_->move_to(cx, cy);
}

void HostApp::click_at_center() {
    const core::Recti b = primary_bounds();
    const std::int32_t cx = b.x + b.w / 2;
    const std::int32_t cy = b.y + b.h / 2;

    DZ_WARN("selftest: left-clicking at (%d, %d) — this clicks whatever is there", cx, cy);
    injector_->button(proto::MouseButton::Left, true, cx, cy);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    injector_->button(proto::MouseButton::Left, false, cx, cy);
}

bool HostApp::accuracy_ok() const {
    return accuracy_.ran && accuracy_.failures == 0 && accuracy_.max_error_px <= 1.0;
}

void HostApp::run_accuracy_selftest() {
    const core::Recti b = layout_.virtual_bounds;
    if (b.w < 2 || b.h < 2) {
        DZ_ERROR("selftest: virtual bounds are degenerate (%dx%d)", b.w, b.h);
        return;
    }

    std::int32_t restore_x = 0;
    std::int32_t restore_y = 0;
    const bool can_restore = injector_->cursor_pos(restore_x, restore_y);

    AccuracyResult result;
    double error_sum = 0.0;

    constexpr int kSteps = 5;
    for (int iy = 0; iy < kSteps; ++iy) {
        for (int ix = 0; ix < kSteps; ++ix) {
            // Sample the full span including both edges, where the rounding is
            // most likely to be wrong.
            const std::int32_t target_x = b.x + static_cast<std::int32_t>(
                                                    std::llround((b.w - 1.0) * ix / (kSteps - 1)));
            const std::int32_t target_y = b.y + static_cast<std::int32_t>(
                                                    std::llround((b.h - 1.0) * iy / (kSteps - 1)));

            if (!injector_->move_to(target_x, target_y)) {
                ++result.failures;
                continue;
            }
            std::this_thread::sleep_for(kCursorSettleDelay);

            std::int32_t got_x = 0;
            std::int32_t got_y = 0;
            if (!injector_->cursor_pos(got_x, got_y)) {
                ++result.failures;
                continue;
            }

            const double dx = static_cast<double>(got_x - target_x);
            const double dy = static_cast<double>(got_y - target_y);
            const double err = std::sqrt(dx * dx + dy * dy);

            ++result.samples;
            error_sum += err;
            if (err > result.max_error_px) {
                result.max_error_px = err;
                result.worst_x = target_x;
                result.worst_y = target_y;
            }
        }
    }

    if (result.samples > 0) {
        result.mean_error_px = error_sum / result.samples;
    }
    result.ran = true;
    accuracy_ = result;

    if (can_restore) {
        injector_->move_to(restore_x, restore_y);
    }

    const auto level = result.max_error_px <= 1.0 ? core::LogLevel::Info : core::LogLevel::Warn;
    core::log_printf(level,
                     "selftest: %d samples, max error %.2f px (at %d, %d), mean %.3f px, "
                     "%d failure(s)",
                     result.samples, result.max_error_px, result.worst_x, result.worst_y,
                     result.mean_error_px, result.failures);
}

// ---------------------------------------------------------------------------

void HostApp::draw_ui() {
    draw_status_panel();
    draw_log_panel();
}

void HostApp::draw_status_panel() {
    ImGui::Begin("Digitiz Host");

    // --- the big switch ---
    const bool was_enabled = enabled_;
    if (enabled_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.32f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.20f, 0.20f, 1.0f));
    }
    if (ImGui::Button(enabled_ ? "INJECTION: ON" : "INJECTION: OFF", ImVec2(-1.0f, 44.0f))) {
        enabled_ = !enabled_;
    }
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("While off, pointer messages are received and counted but never injected.");

    if (enabled_ != was_enabled) {
        {
            std::lock_guard lock(pipeline_mutex_);
            pipeline_->set_enabled(enabled_);
        }
        if (transport_ && session_active_) {
            send_host_state();
        }
    }

    ImGui::Spacing();

    draw_connection_panel();

    // --- display ---
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        const core::Recti& v = layout_.virtual_bounds;
        ImGui::Text("Virtual desktop: %d x %d at (%d, %d)", v.w, v.h, v.x, v.y);

        if (ImGui::BeginTable("monitors", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Device");
            ImGui::TableSetupColumn("Bounds");
            ImGui::TableSetupColumn("DPI");
            ImGui::TableSetupColumn("Primary");
            ImGui::TableHeadersRow();

            for (const MonitorInfo& m : layout_.monitors) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%dx%d @ (%d, %d)", m.bounds.w, m.bounds.h, m.bounds.x, m.bounds.y);
                ImGui::TableNextColumn();
                ImGui::Text("%u", m.dpi);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.primary ? "yes" : "");
            }
            ImGui::EndTable();
        }
    }

    // --- pipeline ---
    if (ImGui::CollapsingHeader("Pointer pipeline", ImGuiTreeNodeFlags_DefaultOpen)) {
        PointerPipeline::Stats s;
        bool stroke = false;
        {
            std::lock_guard lock(pipeline_mutex_);
            s = pipeline_->stats();
            stroke = pipeline_->stroke_active();
        }

        ImGui::Text("received %llu   injected %llu", static_cast<unsigned long long>(s.received),
                    static_cast<unsigned long long>(s.injected));
        ImGui::Text("dropped (off) %llu   protocol errors %llu   inject failures %llu",
                    static_cast<unsigned long long>(s.dropped_disabled),
                    static_cast<unsigned long long>(s.protocol_errors),
                    static_cast<unsigned long long>(s.inject_failures));
        ImGui::Text("stroke: %s   clamped coords: %llu", stroke ? "ACTIVE" : "idle",
                    static_cast<unsigned long long>(injector_->clamped_count()));
        if (ImGui::SmallButton("Reset stats")) {
            std::lock_guard lock(pipeline_mutex_);
            pipeline_->reset_stats();
        }
    }

    draw_selftest_panel();

    ImGui::End();
}

void HostApp::draw_connection_panel() {
    if (!ImGui::CollapsingHeader("Connection", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (!transport_) {
        ImGui::TextDisabled("Transport not started.");
        return;
    }

    const TransportStatus st = transport_->status();

    ImVec4 color(0.70f, 0.72f, 0.78f, 1.0f);
    switch (st.state) {
    case TransportState::Connected:
        color = ImVec4(0.45f, 0.90f, 0.50f, 1.0f);
        break;
    case TransportState::NoAdb:
    case TransportState::Unauthorized:
        color = ImVec4(1.00f, 0.45f, 0.45f, 1.0f);
        break;
    default:
        break;
    }

    ImGui::TextColored(color, "%s", to_string(st.state));
    if (!st.detail.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("- %s", st.detail.c_str());
    }

    if (!st.device_serial.empty()) {
        ImGui::Text("Device: %s  %s", st.device_serial.c_str(), st.device_model.c_str());
    }
    if (st.host_port != 0) {
        ImGui::Text("Tunnel: device tcp:%d -> host tcp:%d", kDevicePort, st.host_port);
    }

    {
        std::lock_guard lock(session_mutex_);
        if (guest_known_) {
            ImGui::Text("Guest: \"%s\"  %d x %d  density %.2f", guest_.device.c_str(),
                        guest_.screen_w, guest_.screen_h, static_cast<double>(guest_.density));
        }
        if (rtt_ms_ >= 0.0) {
            ImGui::Text("RTT: %.2f ms", rtt_ms_);
        } else if (st.state == TransportState::Connected) {
            ImGui::TextDisabled("RTT: waiting for the first PONG");
        }

        ImGui::Separator();

        if (link_latency_.empty()) {
            ImGui::TextDisabled("Latency: draw on the phone to measure");
        } else {
            // Max is the number that matters: an occasional slow event is what
            // the hand notices, not the average.
            ImGui::Text("Event to host:  %.1f ms avg   %.1f min   %.1f max",
                        link_latency_.average(), link_latency_.min(), link_latency_.max());
            ImGui::SetItemTooltip(
                "From the timestamp Android stamped on the touch to the moment this host decoded "
                "it: the phone's input delivery plus the USB tunnel.\n"
                "Does not include touchscreen scan-out, which happens before that timestamp, so "
                "true finger-to-cursor latency is somewhat higher.");
        }
        if (!inject_latency_.empty()) {
            ImGui::Text("Injection:      %.0f us avg   %.0f us max",
                        inject_latency_.average() * 1000.0, inject_latency_.max() * 1000.0);
        }
        if (clock_.ready()) {
            ImGui::TextDisabled("clock offset %.1f ms, best RTT %.2f ms",
                                clock_.offset_us() / 1000.0,
                                static_cast<double>(clock_.best_rtt_us()) / 1000.0);
        }
    }

    ImGui::Text("rx %llu msg / %llu B    tx %llu msg / %llu B",
                static_cast<unsigned long long>(st.rx_messages),
                static_cast<unsigned long long>(st.rx_bytes),
                static_cast<unsigned long long>(st.tx_messages),
                static_cast<unsigned long long>(st.tx_bytes));
    ImGui::Text("sessions %llu    framer resync %llu B",
                static_cast<unsigned long long>(st.sessions),
                static_cast<unsigned long long>(st.resync_bytes));

    if (st.state == TransportState::Connected && ImGui::SmallButton("Drop session")) {
        transport_->drop_session();
    }
}

void HostApp::draw_selftest_panel() {
    if (!ImGui::CollapsingHeader("Self-test", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::TextDisabled("Runs without a phone or transport.");

    if (ImGui::Button("Move cursor to center")) {
        move_cursor_to_center();
    }
    ImGui::SameLine();
    if (ImGui::Button("Left-click at center")) {
        click_at_center();
    }
    ImGui::SetItemTooltip("Clicks whatever window happens to be at the center of the primary "
                          "monitor.");

    if (ImGui::Button("Run coordinate accuracy sweep")) {
        run_accuracy_selftest();
    }
    ImGui::SetItemTooltip("Moves the cursor to 25 points across the virtual desktop and compares "
                          "against the position the OS reports back. Restores the cursor after.");

    if (accuracy_.ran) {
        const bool good = accuracy_.max_error_px <= 1.0 && accuracy_.failures == 0;
        ImGui::TextColored(good ? ImVec4(0.45f, 0.90f, 0.50f, 1.0f)
                                : ImVec4(1.00f, 0.78f, 0.35f, 1.0f),
                           "%d samples | max %.2f px | mean %.3f px | %d failure(s)",
                           accuracy_.samples, accuracy_.max_error_px, accuracy_.mean_error_px,
                           accuracy_.failures);
        if (!good) {
            ImGui::TextDisabled("Worst point: (%d, %d)", accuracy_.worst_x, accuracy_.worst_y);
        }
    }
}

void HostApp::draw_log_panel() {
    ImGui::Begin("Console");

    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("Level", &log_min_level_, "Trace\0Debug\0Info\0Warn\0Error\0");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &log_autoscroll_);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        log_->clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        std::string all;
        log_->for_each([&](const LogEntry& e) {
            char prefix[48];
            std::snprintf(prefix, sizeof(prefix), "[%9.3f] %-5s ", e.t_sec,
                          core::to_string(e.level));
            all += prefix;
            all += e.text;
            all += '\n';
        });
        ImGui::SetClipboardText(all.c_str());
    }

    ImGui::Separator();

    if (ImGui::BeginChild("log_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const auto minimum = static_cast<core::LogLevel>(log_min_level_);

        log_->for_each([&](const LogEntry& e) {
            if (e.level < minimum) {
                return;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, level_color(e.level));
            ImGui::Text("[%9.3f] %-5s %s", e.t_sec, core::to_string(e.level), e.text.c_str());
            ImGui::PopStyleColor();
        });

        if (log_autoscroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace digitiz::host
