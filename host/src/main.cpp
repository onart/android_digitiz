#include <cstdio>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <digitiz/core/log.hpp>

#include "app/HostApp.hpp"
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
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--selftest") {
            selftest_only = true;
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
