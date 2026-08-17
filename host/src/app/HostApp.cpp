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

void HostApp::tick() {
    refresh_layout(false);
}

void HostApp::shutdown() {
    if (pipeline_) {
        // Whatever else happens, do not leave a mouse button held on the
        // user's desktop.
        pipeline_->end_session();
    }
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
        pipeline_->set_enabled(enabled_);
    }

    ImGui::Spacing();

    // --- transport (phase 3) ---
    if (ImGui::CollapsingHeader("Connection", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("ADB transport lands in phase 3.");
    }

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
        const PointerPipeline::Stats& s = pipeline_->stats();
        ImGui::Text("received %llu   injected %llu", static_cast<unsigned long long>(s.received),
                    static_cast<unsigned long long>(s.injected));
        ImGui::Text("dropped (off) %llu   protocol errors %llu   inject failures %llu",
                    static_cast<unsigned long long>(s.dropped_disabled),
                    static_cast<unsigned long long>(s.protocol_errors),
                    static_cast<unsigned long long>(s.inject_failures));
        ImGui::Text("stroke: %s   clamped coords: %llu",
                    pipeline_->stroke_active() ? "ACTIVE" : "idle",
                    static_cast<unsigned long long>(injector_->clamped_count()));
        if (ImGui::SmallButton("Reset stats")) {
            pipeline_->reset_stats();
        }
    }

    draw_selftest_panel();

    ImGui::End();
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
