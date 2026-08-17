#pragma once

// Host application state and UI.
//
// Milestone 1 scope: the on/off toggle, the console, and enough self-test to
// prove input injection works before any transport exists. Phase 3 hangs the
// ADB transport off this.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "display/DisplayInfo.hpp"
#include "input/InputInjector.hpp"
#include "input/PointerPipeline.hpp"
#include "ui/LogStore.hpp"

namespace digitiz::host {

class HostApp {
public:
    explicit HostApp(LogStore& log);

    bool init();
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
    void draw_selftest_panel();
    void draw_log_panel();

    void move_cursor_to_center();
    void click_at_center();

    core::Recti primary_bounds() const;

    LogStore* log_;
    std::unique_ptr<IDisplayInfo> display_;
    std::unique_ptr<IInputInjector> injector_;
    std::unique_ptr<PointerPipeline> pipeline_;

    DisplayLayout layout_;
    std::chrono::steady_clock::time_point last_layout_query_{};

    bool enabled_ = false;
    AccuracyResult accuracy_;

    // Log panel state.
    int log_min_level_ = static_cast<int>(core::LogLevel::Debug);
    bool log_autoscroll_ = true;
};

} // namespace digitiz::host
