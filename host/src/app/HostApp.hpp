#pragma once

// Host application state and UI.
//
// Milestone 1 scope: the on/off toggle, the console, and enough self-test to
// prove input injection works before any transport exists. Phase 3 hangs the
// ADB transport off this.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "app/LatencyStats.hpp"
#include "display/DisplayInfo.hpp"
#include "input/InputInjector.hpp"
#include "input/PointerPipeline.hpp"
#include "transport/AdbTransport.hpp"
#include "ui/LogStore.hpp"

namespace digitiz::host {

class HostApp {
public:
    explicit HostApp(LogStore& log);

    bool init();

    // Separate from init() so `--selftest` can exercise injection without
    // touching adb or opening a socket.
    bool start_transport();

    void tick(); // per-frame housekeeping
    void draw_ui();
    void shutdown();

    // Sweeps the cursor across the virtual desktop and compares against the
    // position the OS reports back. Exposed so `--selftest` can run it without
    // a window. Restores the cursor when done; injects no clicks.
    void run_accuracy_selftest();

    // True when the last sweep stayed within a pixel with no failures.
    bool accuracy_ok() const;

private:
    struct AccuracyResult {
        int samples = 0;
        int failures = 0;
        double max_error_px = 0.0;
        double mean_error_px = 0.0;
        std::int32_t worst_x = 0;
        std::int32_t worst_y = 0;
        bool ran = false;
    };

    void refresh_layout(bool force);
    void draw_status_panel();
    void draw_connection_panel();
    void draw_smoothing_panel();
    void draw_selftest_panel();
    void draw_log_panel();

    void move_cursor_to_center();
    void click_at_center();

    core::Recti primary_bounds() const;

    // --- called on the transport thread ---
    void on_transport_message(proto::MsgType type, std::span<const std::byte> payload);
    void on_transport_connect();
    void on_transport_disconnect();

    // --- called on the UI thread ---
    void pump_session();
    void send_hello_ack();
    void send_host_state();

    LogStore* log_;
    std::unique_ptr<IDisplayInfo> display_;
    std::unique_ptr<IInputInjector> injector_;
    std::unique_ptr<PointerPipeline> pipeline_;
    std::unique_ptr<AdbTransport> transport_;

    // Pointer messages are injected straight off the transport thread — an
    // extra hop would add latency to the one path where it is most visible.
    // This guards the pipeline against the UI thread reading its stats.
    mutable std::mutex pipeline_mutex_;

    // Guards the handshake/heartbeat fields shared with the transport thread.
    mutable std::mutex session_mutex_;
    proto::Hello guest_;
    bool guest_known_ = false;
    bool hello_ack_pending_ = false;
    double rtt_ms_ = -1.0;
    int unanswered_pings_ = 0;

    // Guest timestamps are on the phone's monotonic clock, which shares no
    // epoch with ours, so they have to be translated before they mean anything.
    // Pointer messages in the current stroke. The decimation settings on the
    // guest are tuned by watching this.
    int stroke_samples_ = 0;
    std::uint64_t smoothed_before_stroke_ = 0;

    ClockSync clock_;
    // Finger event on the phone -> message decoded here.
    LatencyStats link_latency_;
    // Time spent inside the injector, measured on this side only.
    LatencyStats inject_latency_;
    // Gap between arriving pointer samples. Smoothing adds exactly one of
    // these, so this is what the UI reports as its cost.
    LatencyStats sample_interval_;
    std::uint64_t last_sample_us_ = 0;

    std::atomic<bool> session_active_{false};
    std::chrono::steady_clock::time_point last_ping_{};

    // The layout is written by the UI thread and read by the transport thread
    // on every pointer event, to decide whether the point is on a screen.
    mutable std::mutex layout_mutex_;
    DisplayLayout layout_;
    std::chrono::steady_clock::time_point last_layout_query_{};

    bool enabled_ = false;
    AccuracyResult accuracy_;

    // Log panel state.
    int log_min_level_ = static_cast<int>(core::LogLevel::Debug);
    bool log_autoscroll_ = true;
};

} // namespace digitiz::host
