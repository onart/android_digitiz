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
    // Uncompressed bytes a batch may spend. The cap is on the uncompressed
    // size because that is the part known before anything is compressed,
    // which is what makes it a budget rather than a hope.
    void set_budget_bytes(int bytes) noexcept { budget_bytes_ = bytes; }
    int budget_bytes() const noexcept { return budget_bytes_; }
    int budget_tiles() const noexcept;

private:
    bool ensure_source();
    // True when a frame was acquired and is still held, which is the only
    // time its pixels can be read.
    bool capture();
    void send_batch();
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
    int budget_bytes_ = 384 * 1024;
    std::chrono::steady_clock::time_point last_frame_{};

    // The captured region, read back once per batch. This is the GPU-to-CPU
    // readback the design exists to avoid, and the first thing to replace with
    // a compute shader that compresses in place.
    std::vector<std::uint8_t> region_pixels_;
    int region_stride_ = 0;
    bool region_valid_ = false;

    std::vector<std::uint8_t> tile_pixels_;
    std::vector<std::uint8_t> payload_;
    std::vector<std::uint8_t> compressed_;
    std::vector<std::uint16_t> selected_;

    Sink sink_;
    ReadyTest ready_;
    Stats stats_;
};

} // namespace digitiz::host
