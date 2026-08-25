#include <cstdio>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <digitiz/core/log.hpp>

#include "app/HostApp.hpp"
#include "screen/CaptureProbe.hpp"
#include "screen/Etc2Conformance.hpp"
#include "screen/FrameProbe.hpp"
#include "ui/ImGuiShell.hpp"
#include "ui/LogStore.hpp"

namespace {

void enable_dpi_awareness() {
#ifdef _WIN32
    // Must happen before any window exists. Without it GetSystemMetrics reports
    // logical pixels on a scaled display and every injected coordinate is off.
    if (!::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        std::fprintf(stderr, "warning: could not set per-monitor-v2 DPI awareness (%lu)\n",
                     ::GetLastError());
    }
#endif
}

} // namespace

int main(int argc, char** argv) {
    enable_dpi_awareness();

    bool selftest_only = false;
    bool capture_test = false;
    bool etc2_test = false;
    bool frame_test = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--selftest") {
            selftest_only = true;
        } else if (arg == "--capture-test") {
            capture_test = true;
        } else if (arg == "--etc2-test") {
            etc2_test = true;
        } else if (arg == "--frame-test") {
            frame_test = true;
        }
    }

    digitiz::host::LogStore log_store;

    digitiz::core::set_log_level(digitiz::core::LogLevel::Debug);
    digitiz::core::set_log_sink([&log_store](digitiz::core::LogLevel level, std::string_view text) {
        log_store.push(level, text);
        std::fprintf(stderr, "%-5s %.*s\n", digitiz::core::to_string(level),
                     static_cast<int>(text.size()), text.data());
    });

    DZ_INFO("digitiz host starting");

    if (frame_test) {
        return digitiz::host::run_frame_probe(6, 2, "frame_probe.bmp") ? 0 : 1;
    }

    if (etc2_test) {
        return digitiz::host::run_etc2_conformance() ? 0 : 1;
    }

    if (capture_test) {
        // Headless: measures what the desktop actually reports as changed,
        // which is what the tile scheduler is built on top of.
        digitiz::host::CaptureProbeResult result;
        const bool ok =
            digitiz::host::run_capture_probe(8, "capture_probe.bmp", result);
        return ok ? 0 : 1;
    }

    if (selftest_only) {
        // Headless: verify coordinate injection without opening a window.
        digitiz::host::HostApp app(log_store);
        if (!app.init()) {
            return 1;
        }
        app.run_accuracy_selftest();
        const bool ok = app.accuracy_ok();
        app.shutdown();
        DZ_INFO("selftest %s", ok ? "PASSED" : "FAILED");
        return ok ? 0 : 1;
    }

    digitiz::host::ImGuiShell shell;
    if (!shell.init("Digitiz Host", 900, 700)) {
        return 1;
    }

    digitiz::host::HostApp app(log_store);
    if (!app.init()) {
        return 1;
    }
    app.start_transport();

    while (shell.begin_frame()) {
        app.tick();
        app.draw_ui();
        shell.end_frame();
    }

    app.shutdown();
    shell.shutdown();

    DZ_INFO("digitiz host stopped");
    return 0;
}
