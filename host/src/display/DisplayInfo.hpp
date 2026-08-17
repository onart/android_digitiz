#pragma once

// Where the PC's pixels are. Everything downstream speaks virtual-desktop
// coordinates, and this is what defines that space.

#include <memory>
#include <string>
#include <vector>

#include <digitiz/core/geometry.hpp>

namespace digitiz::host {

struct MonitorInfo {
    core::Recti bounds; // virtual-desktop coordinates
    std::uint32_t dpi = 96;
    bool primary = false;
    std::string name;

    bool operator==(const MonitorInfo&) const = default;
};

struct DisplayLayout {
    // Union of all monitors. x/y go negative when a monitor sits left of or
    // above the primary one, which is the normal multi-monitor case.
    core::Recti virtual_bounds;
    std::vector<MonitorInfo> monitors;

    bool operator==(const DisplayLayout&) const = default;
};

class IDisplayInfo {
public:
    virtual ~IDisplayInfo() = default;
    virtual DisplayLayout query() = 0;
};

// Returns nullptr on platforms without an implementation yet.
std::unique_ptr<IDisplayInfo> make_display_info();

} // namespace digitiz::host
