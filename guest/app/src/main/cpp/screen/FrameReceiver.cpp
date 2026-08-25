#include "screen/FrameReceiver.hpp"

#include <cstring>

#include <zstd.h>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

// The host only ever has one batch in flight, so this should never be reached.
// It exists because the network thread keeps running while the render thread
// does not -- the surface goes away when the app is backgrounded, and without
// a ceiling the queue would grow for as long as the user is elsewhere.
//
// Dropping loses tiles, which the host believes it has already delivered. That
// is survivable because the guest re-sends VIEWPORT_REQ when it comes back,
// and a viewport change makes the host treat every tile as unsent again.
constexpr std::size_t kMaxQueued = 8;

} // namespace

void FrameReceiver::on_message(proto::MsgType type, std::span<const std::byte> payload) {
    if (type == proto::MsgType::FrameInfo) {
        proto::FrameInfo info;
        if (!proto::decode(payload, info)) {
            std::lock_guard lock(mutex_);
            ++stats_.malformed;
            return;
        }
        begin(info);
    } else if (type == proto::MsgType::FrameData) {
        proto::FrameData chunk;
        if (!proto::decode(payload, chunk)) {
            std::lock_guard lock(mutex_);
            ++stats_.malformed;
            return;
        }
        append(chunk);
    }
}

void FrameReceiver::reset() {
    std::lock_guard lock(mutex_);
    have_info_ = false;
    received_ = 0;
    compressed_.clear();
    ready_.clear();
}

void FrameReceiver::take(std::vector<FrameBatch>& out) {
    out.clear();
    std::lock_guard lock(mutex_);
    out.swap(ready_);
}

FrameReceiver::Stats FrameReceiver::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void FrameReceiver::begin(const proto::FrameInfo& info) {
    std::lock_guard lock(mutex_);
    // A batch that never completed is abandoned here rather than kept: the
    // host has moved on, and half a batch cannot be drawn.
    info_ = info;
    compressed_.assign(info.payload_bytes, 0);
    received_ = 0;
    have_info_ = info.payload_bytes > 0;
}

void FrameReceiver::append(const proto::FrameData& chunk) {
    proto::FrameInfo info;
    std::vector<std::uint8_t> compressed;
    {
        std::lock_guard lock(mutex_);
        if (!have_info_) {
            return;
        }
        if (chunk.seq != info_.seq) {
            // Left over from a batch that was replaced. Not an error: it is
            // what a viewport change mid-transfer looks like.
            ++stats_.stray_seq;
            return;
        }
        if (chunk.offset + chunk.bytes.size() > compressed_.size()) {
            ++stats_.malformed;
            return;
        }
        std::memcpy(compressed_.data() + chunk.offset, chunk.bytes.data(), chunk.bytes.size());
        received_ += chunk.bytes.size();
        if (received_ < compressed_.size()) {
            return;
        }

        // Complete. Lift it out from under the lock before decompressing.
        have_info_ = false;
        info = info_;
        compressed = std::move(compressed_);
        compressed_.clear();
    }
    finish(info, compressed);
}

void FrameReceiver::finish(const proto::FrameInfo& info,
                           const std::vector<std::uint8_t>& compressed) {
    if (info.format != proto::FrameFormat::Etc2Rgb8) {
        // Nothing else is implemented, and painting some other format as ETC2
        // would look like a decoder bug rather than a missing feature.
        DZ_WARN("screen: host sent %s, which this build cannot show",
                proto::to_string(info.format));
        std::lock_guard lock(mutex_);
        ++stats_.malformed;
        return;
    }

    FrameBatch batch;
    batch.geometry = proto::make_geometry(core::Recti{info.x, info.y, info.w, info.h}, info.out_w,
                                          info.out_h, info.tile);
    if (!batch.geometry.valid()) {
        std::lock_guard lock(mutex_);
        ++stats_.malformed;
        return;
    }

    batch.blocks.resize(info.raw_bytes);
    const std::size_t n = ZSTD_decompress(batch.blocks.data(), batch.blocks.size(),
                                          compressed.data(), compressed.size());
    if (ZSTD_isError(n) || n != batch.blocks.size()) {
        DZ_WARN("screen: batch %u would not decompress (%zu bytes -> %u expected)", info.seq,
                compressed.size(), info.raw_bytes);
        std::lock_guard lock(mutex_);
        ++stats_.malformed;
        return;
    }

    // Every byte has to be spoken for. A leftover means the two sides disagree
    // about how big a tile is, which would put every tile after the first one
    // somewhere else -- silently, because the bytes still decode.
    std::size_t needed = 0;
    for (const std::uint16_t index : info.tiles) {
        const core::Recti rect = batch.geometry.tile_output_rect(static_cast<int>(index));
        if (rect.w <= 0 || rect.h <= 0) {
            std::lock_guard lock(mutex_);
            ++stats_.malformed;
            return;
        }
        needed += proto::etc2_size(rect.w, rect.h);
    }
    if (needed != batch.blocks.size()) {
        DZ_WARN("screen: batch %u carries %zu block bytes where %zu tiles need %zu", info.seq,
                batch.blocks.size(), info.tiles.size(), needed);
        std::lock_guard lock(mutex_);
        ++stats_.malformed;
        return;
    }

    batch.tiles = info.tiles;
    const std::size_t tile_count = batch.tiles.size();
    const std::size_t wire_bytes = compressed.size();

    std::lock_guard lock(mutex_);
    if (ready_.size() >= kMaxQueued) {
        ready_.erase(ready_.begin());
        ++stats_.dropped;
    }
    ready_.push_back(std::move(batch));
    ++stats_.batches;
    stats_.tiles += tile_count;
    stats_.bytes += wire_bytes;
}

} // namespace digitiz::guest
