#ifdef _WIN32

#include "display/DisplayInfo.hpp"

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <shellscalingapi.h>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

namespace {

std::string narrow(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

BOOL CALLBACK enum_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM user) {
    auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(user);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(monitor, &mi)) {
        return TRUE; // skip this one, keep enumerating
    }

    MonitorInfo info;
    info.bounds = core::Recti{
        mi.rcMonitor.left,
        mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
    };
    info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.name = narrow(mi.szDevice);

    UINT dpi_x = 96;
    UINT dpi_y = 96;
    if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
        info.dpi = dpi_x;
    }

    monitors->push_back(std::move(info));
    return TRUE;
}

class Win32DisplayInfo final : public IDisplayInfo {
public:
    DisplayLayout query() override {
        DisplayLayout layout;

        layout.virtual_bounds = core::Recti{
            ::GetSystemMetrics(SM_XVIRTUALSCREEN),
            ::GetSystemMetrics(SM_YVIRTUALSCREEN),
            ::GetSystemMetrics(SM_CXVIRTUALSCREEN),
            ::GetSystemMetrics(SM_CYVIRTUALSCREEN),
        };

        ::EnumDisplayMonitors(nullptr, nullptr, &enum_monitor,
                              reinterpret_cast<LPARAM>(&layout.monitors));

        // Primary first, then left-to-right. Keeps the UI table stable across
        // queries; EnumDisplayMonitors makes no ordering promise.
        std::stable_sort(layout.monitors.begin(), layout.monitors.end(),
                         [](const MonitorInfo& a, const MonitorInfo& b) {
                             if (a.primary != b.primary) {
                                 return a.primary;
                             }
                             return a.bounds.x < b.bounds.x;
                         });

        return layout;
    }
};

} // namespace

std::unique_ptr<IDisplayInfo> make_display_info() {
    return std::make_unique<Win32DisplayInfo>();
}

} // namespace digitiz::host

#endif // _WIN32
