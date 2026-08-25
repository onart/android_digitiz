#include "screen/ViewportGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace digitiz::host {

namespace {

int round_up_to(int v, int multiple) noexcept {
    if (multiple <= 0) {
        return v;
    }
    return ((v + multiple - 1) / multiple) * multiple;
}

} // namespace

core::Recti ViewportGeometry::tile_output_rect(int index) const noexcept {
    if (tile <= 0 || index < 0 || index >= tile_count()) {
        return {};
    }
    const int col = index % cols();
    const int row = index / cols();
    const int x = col * tile;
    const int y = row * tile;
    return core::Recti{x, y, std::min(tile, out_w - x), std::min(tile, out_h - y)};
}

core::Recti ViewportGeometry::tile_source_rect(int index) const noexcept {
    const core::Recti out = tile_output_rect(index);
    if (out.w <= 0 || out.h <= 0) {
        return {};
    }

    // Mapped by edges rather than by origin-plus-size, so neighbouring tiles
    // share an edge exactly and no desktop column falls between two of them.
    const auto edge = [](int v, int from, int to) {
        return static_cast<int>(static_cast<long long>(v) * to / std::max(from, 1));
    };
    const int x0 = edge(out.x, out_w, region.w);
    const int x1 = edge(out.x + out.w, out_w, region.w);
    const int y0 = edge(out.y, out_h, region.h);
    const int y1 = edge(out.y + out.h, out_h, region.h);

    return core::Recti{region.x + x0, region.y + y0, std::max(x1 - x0, 1), std::max(y1 - y0, 1)};
}

core::Recti ViewportGeometry::tiles_touching(core::Recti desktop) const noexcept {
    if (!valid()) {
        return {};
    }

    // Into region-relative desktop pixels, clipped to the region.
    const int rx0 = std::max(desktop.x - region.x, 0);
    const int ry0 = std::max(desktop.y - region.y, 0);
    const int rx1 = std::min(desktop.x + desktop.w - region.x, region.w);
    const int ry1 = std::min(desktop.y + desktop.h - region.y, region.h);
    if (rx1 <= rx0 || ry1 <= ry0) {
        return {};
    }

    // Into the encoded surface. The far edge rounds up so a partly covered
    // output pixel still counts as touched.
    const auto to_out = [](int v, int from, int to, bool up) {
        const long long scaled = static_cast<long long>(v) * to;
        const long long div = std::max(from, 1);
        return static_cast<int>(up ? (scaled + div - 1) / div : scaled / div);
    };
    const int ox0 = to_out(rx0, region.w, out_w, false);
    const int oy0 = to_out(ry0, region.h, out_h, false);
    const int ox1 = to_out(rx1, region.w, out_w, true);
    const int oy1 = to_out(ry1, region.h, out_h, true);

    // Into tiles, outwards: a tile is dirty if any part of it is.
    const int tx0 = std::clamp(ox0 / tile, 0, cols() - 1);
    const int ty0 = std::clamp(oy0 / tile, 0, rows() - 1);
    const int tx1 = std::clamp((ox1 + tile - 1) / tile, 1, cols());
    const int ty1 = std::clamp((oy1 + tile - 1) / tile, 1, rows());

    return core::Recti{tx0, ty0, std::max(tx1 - tx0, 1), std::max(ty1 - ty0, 1)};
}

ViewportGeometry make_geometry(core::Recti region, int out_w, int out_h, int tile) noexcept {
    ViewportGeometry g;
    if (region.w <= 0 || region.h <= 0) {
        return g;
    }

    // Asking for nothing means asking for the region at its own size.
    if (out_w <= 0) {
        out_w = region.w;
    }
    if (out_h <= 0) {
        out_h = region.h;
    }
    if (tile <= 0) {
        tile = 64;
    }

    g.region = region;
    // Rounded up rather than down: losing the last three columns of the region
    // would be a visible strip of nothing.
    g.out_w = round_up_to(std::clamp(out_w, 4, 8192), 4);
    g.out_h = round_up_to(std::clamp(out_h, 4, 8192), 4);
    g.tile = round_up_to(std::clamp(tile, 8, 256), 4);
    return g;
}

} // namespace digitiz::host
