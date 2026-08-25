#pragma once

// Puts the host's tile batches back together.
//
// The host sends a FRAME_INFO naming which tiles are in the batch, then the
// bytes as FRAME_DATA chunks. This is the other end of that: fill a buffer,
// decompress it once it is full, and hand the result to the render thread.
//
// What it deliberately does not do is turn the blocks into pixels. They are
// ETC2, which the GPU samples directly, so the whole point is that nothing
// decodes them -- see ScreenRenderer for where they end up.
//
// Assembly runs on the network thread; take() and reset() are called from the
// render thread, so the state is under a lock. The decompression deliberately
// is not: it is the one expensive step here, and holding the lock across it
// would put it in the render thread's way once per batch.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include <digitiz/proto/messages.hpp>
#include <digitiz/proto/tiling.hpp>

namespace digitiz::guest {

// One batch, assembled and decompressed, waiting to be handed to the GPU.
//
// It carries its own geometry rather than referring to whatever the guest
// currently wants: a batch that crosses a viewport change belongs to the
// request it was made for, and drawing it against the new one would put it in
// the wrong place.
struct FrameBatch {
    proto::ViewportGeometry geometry;
    std::vector<std::uint16_t> tiles;
    // ETC2 blocks, tiles concatenated in the order of `tiles`.
    std::vector<std::uint8_t> blocks;
};

class FrameReceiver {
public:
    // --- network thread ---

    // Accepts FRAME_INFO and FRAME_DATA; ignores everything else.
    void on_message(proto::MsgType type, std::span<const std::byte> payload);

    // The link went away, or the stream was turned off. Anything half
    // assembled is worthless, and anything the render thread has not picked up
    // describes a session that no longer exists.
    void reset();

    // --- render thread ---

    // Moves out everything completed since the last call, oldest first.
    void take(std::vector<FrameBatch>& out);

    struct Stats {
        std::uint64_t batches = 0;
        std::uint64_t tiles = 0;
        std::uint64_t bytes = 0; // compressed, as they came off the wire
        std::uint64_t malformed = 0;
        std::uint64_t stray_seq = 0;
        std::uint64_t dropped = 0;
    };
    Stats stats() const;

private:
    void begin(const proto::FrameInfo& info);
    void append(const proto::FrameData& chunk);
    // Called with the batch already lifted out from under the lock.
    void finish(const proto::FrameInfo& info, const std::vector<std::uint8_t>& compressed);

    mutable std::mutex mutex_;

    proto::FrameInfo info_{};
    std::vector<std::uint8_t> compressed_;
    std::size_t received_ = 0;
    bool have_info_ = false;

    std::vector<FrameBatch> ready_;
    Stats stats_{};
};

} // namespace digitiz::guest
