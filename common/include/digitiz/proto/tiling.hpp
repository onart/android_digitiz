#pragma once

// How a frame is cut into tiles, and how many bytes each one takes.
//
// This lives in common rather than on the host because both ends have to
// compute it identically. FRAME_INFO carries a list of tile indices and one
// run of block bytes; the guest turns that back into rectangles with the same
// arithmetic the host used to fill it. If the two disagree by a pixel, the
// picture comes out shifted; if they disagree about a tile's byte count, every
// tile after it lands somewhere else. Neither produces an error message, which
// is exactly why the arithmetic is written once.
//
// The three coordinate spaces a frame passes through:
//
//   desktop   what the capture and the dirty rects speak
//   output    the encoded surface, which is the region scaled by whatever
//             resolution ratio the guest asked for
//   tiles     the grid the scheduler works in
//
// Only the host has dirty rects, so only the host calls tiles_touching(); the
// guest uses tile_output_rect() and etc2_size(). Both come from the same
// geometry, built from the same numbers.

#include <cstddef>
#include <cstdint>

#include <digitiz/core/geometry.hpp>

namespace digitiz::proto {

// --- block format ----------------------------------------------------------

inline constexpr int kEtc2BlockSize = 4;
inline constexpr std::size_t kEtc2BlockBytes = 8;

// Bytes a w*h image takes once encoded. Both must be multiples of 4.
//
// Fixed by the dimensions alone, never by the content -- that is the property
// the host's tile budget is built on, and the property that lets the guest
// walk the payload without any per-tile header.
constexpr std::size_t etc2_size(int w, int h) noexcept {
    if (w <= 0 || h <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(w / kEtc2BlockSize) *
           static_cast<std::size_t>(h / kEtc2BlockSize) * kEtc2BlockBytes;
}

// --- tiling ----------------------------------------------------------------

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

} // namespace digitiz::proto
