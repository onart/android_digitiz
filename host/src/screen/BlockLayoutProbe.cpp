// Asks whether the order the blocks are laid out in is worth changing.
//
// A batch's blocks are concatenated tile by tile, so a run of similar content
// across the screen is broken every 64 pixels by the next tile's first row.
// If the tiles of a batch formed a rectangle instead, the blocks could go out
// in that rectangle's own row order and a horizontal run would stay a run --
// which is the sort of thing zstd is paid to notice.
//
// That change costs a protocol change and a different scheduler, so the
// question is what it would buy. This measures the ceiling: the same blocks
// from a real desktop, compressed in both orders.

#include "screen/BlockLayoutProbe.hpp"

#include <cstdint>
#include <vector>

#include <zstd.h>

#include <digitiz/core/log.hpp>

#include "display/DisplayInfo.hpp"
#include "screen/Etc2.hpp"
#include "screen/FrameSource.hpp"
#include "screen/ViewportGeometry.hpp"

namespace digitiz::host {

namespace {

constexpr int kZstdLevel = 1;

std::size_t compressed_size(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out(ZSTD_compressBound(in.size()));
    const std::size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), kZstdLevel);
    return ZSTD_isError(n) ? 0 : n;
}

} // namespace

bool run_block_layout_probe(int divisor) {
    std::unique_ptr<IDisplayInfo> display = make_display_info();
    std::unique_ptr<IFrameSource> source = make_frame_source();
    if (!display || !source || !source->start()) {
        DZ_ERROR("layout probe: no capture backend");
        return false;
    }

    const core::Recti desktop = display->query().virtual_bounds;
    const ViewportGeometry g =
        make_geometry(desktop, desktop.w / std::max(divisor, 1), desktop.h / std::max(divisor, 1),
                      64);
    if (!g.valid()) {
        DZ_ERROR("layout probe: degenerate geometry");
        return false;
    }

    // One frame, however long it takes to arrive.
    FrameUpdate update;
    for (int tries = 0; tries < 200; ++tries) {
        if (source->next(update, 50)) {
            break;
        }
    }
    std::vector<std::uint8_t> pixels;
    int stride = 0;
    if (!source->read(g.region, pixels, stride)) {
        DZ_ERROR("layout probe: nothing to read");
        return false;
    }

    // Every tile, encoded once. Kept per tile so the two orders are the same
    // bytes in a different sequence and nothing else differs.
    const int cols = g.cols();
    const int rows = g.rows();
    std::vector<std::vector<std::uint8_t>> per_tile(static_cast<std::size_t>(cols * rows));
    std::vector<std::uint8_t> tile_px;

    for (int index = 0; index < cols * rows; ++index) {
        const core::Recti out = g.tile_output_rect(index);
        const core::Recti src = g.tile_source_rect(index);
        tile_px.assign(static_cast<std::size_t>(out.w) * out.h * 4, 0);
        for (int y = 0; y < out.h; ++y) {
            const int sy0 = src.y - g.region.y + y * src.h / out.h;
            const int sy1 = std::max(sy0 + 1, src.y - g.region.y + (y + 1) * src.h / out.h);
            for (int x = 0; x < out.w; ++x) {
                const int sx0 = src.x - g.region.x + x * src.w / out.w;
                const int sx1 = std::max(sx0 + 1, src.x - g.region.x + (x + 1) * src.w / out.w);
                int b = 0;
                int gg = 0;
                int r = 0;
                int n = 0;
                for (int sy = sy0; sy < sy1; ++sy) {
                    if (sy < 0 || sy >= g.region.h) {
                        continue;
                    }
                    const std::uint8_t* row = pixels.data() + static_cast<std::size_t>(sy) * stride;
                    for (int sx = sx0; sx < sx1; ++sx) {
                        if (sx < 0 || sx >= g.region.w) {
                            continue;
                        }
                        const std::uint8_t* p = row + static_cast<std::size_t>(sx) * 4;
                        b += p[0];
                        gg += p[1];
                        r += p[2];
                        ++n;
                    }
                }
                n = std::max(n, 1);
                std::uint8_t* dst =
                    tile_px.data() + (static_cast<std::size_t>(y) * out.w + x) * 4;
                dst[0] = static_cast<std::uint8_t>(b / n);
                dst[1] = static_cast<std::uint8_t>(gg / n);
                dst[2] = static_cast<std::uint8_t>(r / n);
                dst[3] = 255;
            }
        }
        if (!etc2_encode(tile_px.data(), out.w, out.h, out.w * 4, per_tile[static_cast<std::size_t>(index)])) {
            DZ_ERROR("layout probe: encode failed on tile %d", index);
            return false;
        }
    }

    // As it goes out now: tile after tile, each tile's blocks in its own
    // reading order.
    std::vector<std::uint8_t> tile_major;
    for (const std::vector<std::uint8_t>& t : per_tile) {
        tile_major.insert(tile_major.end(), t.begin(), t.end());
    }

    // As a rectangle would: one row of blocks across the whole surface, then
    // the next. Same bytes, different sequence.
    std::vector<std::uint8_t> row_major;
    row_major.reserve(tile_major.size());
    const int tile_blocks = g.tile / 4;
    for (int trow = 0; trow < rows; ++trow) {
        const core::Recti any = g.tile_output_rect(trow * cols);
        const int block_rows = any.h / 4;
        for (int br = 0; br < block_rows; ++br) {
            for (int tcol = 0; tcol < cols; ++tcol) {
                const int index = trow * cols + tcol;
                const core::Recti out = g.tile_output_rect(index);
                const int width_blocks = out.w / 4;
                const std::vector<std::uint8_t>& blocks = per_tile[static_cast<std::size_t>(index)];
                const std::size_t offset =
                    static_cast<std::size_t>(br) * width_blocks * kEtc2BlockBytes;
                const std::size_t bytes =
                    static_cast<std::size_t>(width_blocks) * kEtc2BlockBytes;
                if (offset + bytes > blocks.size()) {
                    continue;
                }
                row_major.insert(row_major.end(), blocks.begin() + static_cast<std::ptrdiff_t>(offset),
                                 blocks.begin() + static_cast<std::ptrdiff_t>(offset + bytes));
            }
        }
    }
    (void)tile_blocks;

    if (row_major.size() != tile_major.size()) {
        DZ_ERROR("layout probe: reorder lost bytes (%zu vs %zu)", row_major.size(),
                 tile_major.size());
        return false;
    }

    const std::size_t a = compressed_size(tile_major);
    const std::size_t b = compressed_size(row_major);
    if (a == 0 || b == 0) {
        DZ_ERROR("layout probe: compression failed");
        return false;
    }

    DZ_INFO("layout probe: %dx%d -> %dx%d, %d tiles, %.1f KiB of blocks", g.region.w, g.region.h,
            g.out_w, g.out_h, cols * rows, tile_major.size() / 1024.0);
    DZ_INFO("layout probe: tile order %.1f KiB (%.1fx), row order %.1f KiB (%.1fx)", a / 1024.0,
            static_cast<double>(tile_major.size()) / a, b / 1024.0,
            static_cast<double>(row_major.size()) / b);
    DZ_INFO("layout probe: row order is %+.1f%% of the bytes",
            (static_cast<double>(b) - static_cast<double>(a)) * 100.0 / static_cast<double>(a));

    source->stop();
    return true;
}

} // namespace digitiz::host
