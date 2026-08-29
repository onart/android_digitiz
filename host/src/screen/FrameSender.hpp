#pragma once

// Turns a captured desktop into batches of tiles on the wire.
//
// Everything it needs already exists separately: the capture reports what
// changed, the scheduler decides what to spend the budget on, the block
// encoder makes the bytes, and the send queue keeps them out of the pointer
// path. This is the part that runs them in order.
//
// Two rules it must not break:
//
//   * One frame in flight. Nothing new is queued until the last batch has
//     gone, so a link that cannot keep up produces fewer frames rather than a
//     growing backlog of stale ones.
//   * Tiles are only marked sent once they have actually been queued. A tile
//     carries state -- skipping it leaves that patch of the guest's picture
//     wrong until it is sent again, unlike a video frame that is merely late.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <digitiz/proto/messages.hpp>

#include "screen/FrameSource.hpp"
#include "screen/TileScheduler.hpp"
#include "screen/ViewportGeometry.hpp"

namespace digitiz::host {

class FrameSender {
public:
    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t tiles = 0;
        std::uint64_t bytes = 0;    // after compression, payload only
        std::uint64_t raw_bytes = 0; // before compression
        std::uint64_t skipped_busy = 0; // the previous batch had not drained
        std::uint64_t captures = 0;
        double last_encode_ms = 0.0;

        // Where the time goes, cumulative in microseconds. Split because the
        // three are charged differently: the readback is once per batch and
        // does not grow with the budget, while the downscale and the encode
        // are per tile and do. Which of them dominates decides whether a
        // bigger batch is nearly free or nearly linear.
        std::uint64_t read_us = 0;   // per batch
        std::uint64_t gather_us = 0; // per tile: cut out and box-average
        std::uint64_t etc2_us = 0;   // per tile
        std::uint64_t zstd_us = 0;   // per batch
    };

    // Queues one message of screen data. The caller routes it as bulk.
    using Sink = std::function<void(std::vector<std::byte>)>;
    // Whether the last batch has finished going out.
    using ReadyTest = std::function<bool()>;

    void set_sink(Sink sink, ReadyTest ready) {
        sink_ = std::move(sink);
        ready_ = std::move(ready);
    }

    // Applies what the guest asked for. A request that changes the geometry
    // throws away everything the guest was told before, because tile numbers
    // no longer point at the same places.
    void set_viewport(const proto::ViewportReq& request);
    bool streaming() const noexcept { return streaming_; }

    // Stops sending and forgets the guest's picture, so the next session
    // starts from a full repaint.
    void stop();

    // Called every host tick. Captures if there is anything to capture, and
    // sends a batch if it is time and the last one has drained.
    void tick();

    const Stats& stats() const noexcept { return stats_; }
    const ViewportGeometry& geometry() const noexcept { return geometry_; }
    int dirty_tiles() const noexcept { return scheduler_.dirty_count(); }
    // Tiles one batch may spend on the sweep.
    //
    // Counted in tiles rather than bytes, because ETC2 is a fixed 8:1: N tiles
    // is exactly N * (tile/4)^2 * 8 bytes before compression, known before
    // anything is compressed. That is what makes it a budget rather than a
    // hope, and stating it in bytes only hid the number that matters.
    //
    // It must be smaller than the grid or it is not a cap at all -- a budget
    // of 192 against a 180-tile grid was the same as having none, and it left
    // the sweep with nothing to converge over.
    //
    // The tile under the pen does not come out of this. It rides on top, so
    // the pen and the sweep never take from each other.
    void set_budget_tiles(int tiles) noexcept { budget_tiles_ = tiles > 0 ? tiles : 1; }
    int budget_tiles() const noexcept { return budget_tiles_; }

    // Where the pen is on the desktop, and whether it is touching. Kept as a
    // point rather than a tile number because a viewport change renumbers the
    // tiles, and a stale number would paint the wrong square.
    void set_pen(bool down, std::int32_t pc_x, std::int32_t pc_y) noexcept;

private:
    bool ensure_source();
    // True when a frame was acquired and is still held, which is the only
    // time its pixels can be read.
    bool capture();
    void send_batch();
    // Reads the pixels the selected tiles are cut from -- their bounding box,
    // not the whole region. Sets `read_rect_` to what was actually read.
    bool read_selection();
    // The tile the pen is on, or -1: not touching, not streaming, or touching
    // somewhere outside the region the guest asked for.
    int pen_tile() const noexcept;
    // Copies one tile out of the captured region, scaling if the encoded size
    // differs, into `tile_pixels_`.
    bool gather_tile(int index, int& w, int& h);

    std::unique_ptr<IFrameSource> source_;
    TileScheduler scheduler_;
    ViewportGeometry geometry_;
    proto::ViewportReq request_;
    bool streaming_ = false;
    bool geometry_ready_ = false;

    std::uint32_t seq_ = 0;
    // One. The default spends the link on how fresh the pen's own tile is
    // rather than on how fast the rest catches up, which is the right way
    // round for drawing; raise it when the whole picture matters more.
    int budget_tiles_ = 1;
    bool pen_down_ = false;
    int logged_pen_ = -1;
    std::int32_t pen_x_ = 0;
    std::int32_t pen_y_ = 0;
    std::chrono::steady_clock::time_point last_frame_{};

    // Pixels for the tiles of one batch, read back just before they are
    // encoded. This is the GPU-to-CPU readback the design exists to avoid, and
    // the first thing to replace with a compute shader that compresses in
    // place -- at which point only blocks cross the bus, and `read_rect_` and
    // gather_tile go with it.
    std::vector<std::uint8_t> region_pixels_;
    core::Recti read_rect_{};
    int region_stride_ = 0;

    std::vector<std::uint8_t> tile_pixels_;
    std::vector<std::uint8_t> payload_;
    std::vector<std::uint8_t> compressed_;
    std::vector<std::uint16_t> selected_;

    Sink sink_;
    ReadyTest ready_;
    Stats stats_;
};

} // namespace digitiz::host
