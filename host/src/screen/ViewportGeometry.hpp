#pragma once

// The three coordinate spaces a frame passes through, and the arithmetic
// between them.
//
//   desktop   what the capture and the dirty rects speak
//   output    the encoded surface, which is the region scaled by whatever
//             resolution ratio the user chose
//   tiles     the grid the scheduler works in
//
// Separated out because the conversions are where an off-by-one turns into a
// patch of screen that is subtly in the wrong place, and that is much easier
// to test here than to spot on a phone.

#include <cstdint>

#include <digitiz/core/geometry.hpp>

namespace digitiz::host {

struct ViewportGeometry {
    // The desktop region being served.
    core::Recti region{};
    // Encoded size. Both multiples of four, because block compression needs
    // whole blocks; the host rounds up rather than asking the guest to.
    int out_w = 0;
    int out_h = 0;
    // Tile edge in pixels, a multiple of four and absolute rather than a
    // fraction: a bigger region should mean more tiles, not bigger ones.
    int tile = 0;

    int cols() const noexcept { return tile > 0 ? (out_w + tile - 1) / tile : 0; }
    int rows() const noexcept { return tile > 0 ? (out_h + tile - 1) / tile : 0; }
    int tile_count() const noexcept { return cols() * rows(); }
    bool valid() const noexcept {
        return region.w > 0 && region.h > 0 && out_w > 0 && out_h > 0 && tile > 0 &&
               out_w % 4 == 0 && out_h % 4 == 0 && tile % 4 == 0;
    }

    // Where one tile lands on the encoded surface. The right and bottom edges
    // are short when the surface is not a whole number of tiles across.
    core::Recti tile_output_rect(int index) const noexcept;

    // The desktop pixels a tile is sampled from.
    core::Recti tile_source_rect(int index) const noexcept;

    // Which tiles a desktop rectangle touches, as a rectangle in tile
    // coordinates. Rounded outwards: a tile is dirty if any part of it is.
    core::Recti tiles_touching(core::Recti desktop) const noexcept;
};

// Builds a geometry from what the guest asked for, rounding the encoded size
// up to whole blocks and clamping the tile size to something sane. Returns a
// geometry whose valid() is false if the request cannot be served at all.
ViewportGeometry make_geometry(core::Recti region, int out_w, int out_h, int tile) noexcept;

} // namespace digitiz::host
