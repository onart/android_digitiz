#include <cstdio>
#include <filesystem>
#include <string>
#include <cstdlib>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <digitiz/core/log.hpp>

#include "app/HostApp.hpp"
#include "screen/CaptureProbe.hpp"
#include "screen/BlockLayoutProbe.hpp"
#include "screen/Etc2ComputeTest.hpp"
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

// host.txt sits beside the executable rather than in a user directory: this
// is a tool run out of its own build tree, and a settings file you can see
// next to the binary is easier to reason about than one hidden away.
std::string settings_dir_beside(const char* argv0) {
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::absolute(argv0 ? argv0 : "", ec);
    if (ec || !exe.has_parent_path()) {
        return std::filesystem::current_path(ec).string();
    }
    return exe.parent_path().string();
}

} // namespace

int main(int argc, char** argv) {
    enable_dpi_awareness();

    bool selftest_only = false;
    bool capture_test = false;
    bool etc2_test = false;
    bool gpu_encode_test = false;
    bool layout_test = false;
    int layout_divisor = 2;
    bool frame_test = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--selftest") {
            selftest_only = true;
        } else if (arg == "--capture-test") {
            capture_test = true;
        } else if (arg == "--etc2-test") {
            etc2_test = true;
        } else if (arg == "--gpu-encode-test") {
            gpu_encode_test = true;
        } else if (arg == "--layout-test") {
            layout_test = true;
            // Optional resolution ratio, so the case with the longest runs
            // (no downscale at all) can be asked for too.
            if (i + 1 < argc) {
                const int n = std::atoi(argv[i + 1]);
                if (n > 0) {
                    layout_divisor = n;
                    ++i;
                }
            }
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

    if (layout_test) {
        return digitiz::host::run_block_layout_probe(layout_divisor) ? 0 : 1;
    }

    if (gpu_encode_test) {
        return digitiz::host::run_etc2_compute_test() ? 0 : 1;
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
    if (!app.init(settings_dir_beside(argv[0]))) {
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
