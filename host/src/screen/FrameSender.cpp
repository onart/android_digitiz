#include "screen/FrameSender.hpp"

#include <algorithm>
#include <cstring>

#include <zstd.h>

#include <digitiz/core/log.hpp>

#include "screen/Etc2.hpp"

namespace digitiz::host {

namespace {

// Fast enough to run every frame beside the pointer path, and the difference
// to a higher level is small on data that is already block compressed.
constexpr int kZstdLevel = 1;

} // namespace

void FrameSender::set_viewport(const proto::ViewportReq& request) {
    const ViewportGeometry fresh =
        make_geometry(core::Recti{request.x, request.y, request.w, request.h}, request.out_w,
                      request.out_h, request.tile);

    const bool wants_stream = request.fps > 0 && request.format != proto::FrameFormat::Off;
    if (!wants_stream || !fresh.valid()) {
        if (streaming_) {
            DZ_INFO("screen: stream stopped");
        }
        streaming_ = false;
        request_ = request;
        return;
    }

    // Only ETC2 exists so far. Saying so is better than sending something the
    // guest will decode as noise.
    if (request.format != proto::FrameFormat::Etc2Rgb8) {
        DZ_WARN("screen: %s requested but only ETC2_RGB8 is implemented",
                proto::to_string(request.format));
        streaming_ = false;
        return;
    }

    const bool changed = !geometry_ready_ || fresh.region != geometry_.region ||
                         fresh.out_w != geometry_.out_w || fresh.out_h != geometry_.out_h ||
                         fresh.tile != geometry_.tile;

    request_ = request;
    geometry_ = fresh;
    streaming_ = true;

    if (changed) {
        // Tile numbers no longer mean the same places, so nothing the guest
        // holds can be reused.
        geometry_ = fresh;
        geometry_ready_ = true;
        scheduler_.configure(fresh.cols(), fresh.rows());
        region_valid_ = false;
        pixels_stale_ = true;
        DZ_INFO("screen: %dx%d at (%d, %d) -> %dx%d, %d px tiles (%d), %u fps", fresh.region.w,
                fresh.region.h, fresh.region.x, fresh.region.y, fresh.out_w, fresh.out_h,
                fresh.tile, fresh.tile_count(), request.fps);
    }
}

void FrameSender::stop() {
    streaming_ = false;
    geometry_ready_ = false;
    region_valid_ = false;
    pixels_stale_ = true;
    if (source_) {
        source_->stop();
        source_.reset();
    }
    // The next guest has seen nothing, so it is owed everything.
    scheduler_.configure(0, 0);
}

bool FrameSender::ensure_source() {
    if (source_) {
        return true;
    }
    source_ = make_frame_source();
    if (!source_) {
        DZ_ERROR("screen: no capture backend on this platform");
        streaming_ = false;
        return false;
    }
    if (!source_->start()) {
        source_.reset();
        streaming_ = false;
        return false;
    }
    return true;
}

bool FrameSender::capture() {
    FrameUpdate update;
    // Exactly one acquire per tick, and zero wait, for two reasons. A still
    // screen must not stall the tick; and the frame stays held until the next
    // acquire releases it, so draining until the queue is empty would leave
    // nothing to read the pixels out of -- which is precisely what it did.
    //
    // Nothing is missed by taking one: ticks are far more frequent than
    // batches, each acquire brings its own dirty rects, and the driver merges
    // what happened in between and says so through AccumulatedFrames.
    if (!source_->next(update, 0)) {
        return false;
    }
    ++stats_.captures;

    if (update.full || update.dirty.empty()) {
        scheduler_.mark_all_dirty();
        pixels_stale_ = true;
    } else {
        for (const core::Recti& dirty : update.dirty) {
            const core::Recti tiles = geometry_.tiles_touching(dirty);
            if (tiles.w > 0 && tiles.h > 0) {
                // Something inside the served region moved, so the pixels we
                // are holding are no longer what it looks like.
                pixels_stale_ = true;
            }
            scheduler_.mark_dirty_rect(tiles);
        }
    }
    // Move rects would be handled here, if Windows ever produced any.
    for (const MoveRect& move : update.moves) {
        scheduler_.mark_dirty_rect(geometry_.tiles_touching(move.to));
        pixels_stale_ = true;
    }
    return true;
}

bool FrameSender::gather_tile(int index, int& w, int& h) {
    const core::Recti out = geometry_.tile_output_rect(index);
    const core::Recti src = geometry_.tile_source_rect(index);
    if (out.w <= 0 || out.h <= 0) {
        return false;
    }
    w = out.w;
    h = out.h;
    tile_pixels_.resize(static_cast<std::size_t>(w) * h * 4);

    const core::Recti& region = geometry_.region;
    for (int y = 0; y < h; ++y) {
        // Sample by area rather than by nearest: downscaling text by picking
        // one source pixel per output pixel drops whole strokes, and text is
        // most of what this is for.
        const int sy0 = src.y - region.y + y * src.h / h;
        const int sy1 = std::max(sy0 + 1, src.y - region.y + (y + 1) * src.h / h);
        for (int x = 0; x < w; ++x) {
            const int sx0 = src.x - region.x + x * src.w / w;
            const int sx1 = std::max(sx0 + 1, src.x - region.x + (x + 1) * src.w / w);

            int b = 0;
            int g = 0;
            int r = 0;
            int n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                if (sy < 0 || sy >= region.h) {
                    continue;
                }
                const std::uint8_t* row =
                    region_pixels_.data() + static_cast<std::size_t>(sy) * region_stride_;
                for (int sx = sx0; sx < sx1; ++sx) {
                    if (sx < 0 || sx >= region.w) {
                        continue;
                    }
                    const std::uint8_t* p = row + static_cast<std::size_t>(sx) * 4;
                    b += p[0];
                    g += p[1];
                    r += p[2];
                    ++n;
                }
            }
            if (n == 0) {
                n = 1;
            }
            std::uint8_t* dst =
                tile_pixels_.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            dst[0] = static_cast<std::uint8_t>(b / n);
            dst[1] = static_cast<std::uint8_t>(g / n);
            dst[2] = static_cast<std::uint8_t>(r / n);
            dst[3] = 255;
        }
    }
    return true;
}

void FrameSender::set_pen(bool down, std::int32_t pc_x, std::int32_t pc_y) noexcept {
    if (down != pen_down_) {
        DZ_DEBUG("screen: pen %s at (%d, %d)", down ? "down" : "up", pc_x, pc_y);
    }
    pen_down_ = down;
    pen_x_ = pc_x;
    pen_y_ = pc_y;
}

int FrameSender::pen_tile() const noexcept {
    if (!pen_down_ || !geometry_.valid()) {
        return -1;
    }
    const core::Recti hit = geometry_.tiles_touching(core::Recti{pen_x_, pen_y_, 1, 1});
    if (hit.w <= 0 || hit.h <= 0) {
        return -1;
    }
    return hit.y * geometry_.cols() + hit.x;
}

void FrameSender::send_batch() {
    // Resolved here rather than when the pointer arrived, so that a viewport
    // change between the two cannot leave a tile number meaning a different
    // square than it did.
    const int pen = pen_tile();
    // Only when it moves to a different square, which is a handful of lines
    // per stroke rather than one per batch.
    if (pen != logged_pen_) {
        logged_pen_ = pen;
        DZ_DEBUG("screen: pen tile %d", pen);
    }
    scheduler_.set_focus(pen);
    scheduler_.select(budget_tiles(), selected_);
    if (selected_.empty()) {
        return;
    }

    const auto started = std::chrono::steady_clock::now();

    payload_.clear();
    std::vector<std::uint8_t> blocks;
    std::vector<std::uint16_t> sent;
    sent.reserve(selected_.size());

    for (const std::uint16_t index : selected_) {
        int w = 0;
        int h = 0;
        if (!gather_tile(static_cast<int>(index), w, h)) {
            continue;
        }
        if (!etc2_encode(tile_pixels_.data(), w, h, w * 4, blocks)) {
            continue;
        }
        payload_.insert(payload_.end(), blocks.begin(), blocks.end());
        sent.push_back(index);
    }
    if (sent.empty()) {
        return;
    }

    compressed_.resize(ZSTD_compressBound(payload_.size()));
    const std::size_t n = ZSTD_compress(compressed_.data(), compressed_.size(), payload_.data(),
                                        payload_.size(), kZstdLevel);
    if (ZSTD_isError(n)) {
        DZ_WARN("screen: compression failed (%s)", ZSTD_getErrorName(n));
        return;
    }

    ++seq_;
    proto::FrameInfo info;
    info.seq = seq_;
    info.x = geometry_.region.x;
    info.y = geometry_.region.y;
    info.w = geometry_.region.w;
    info.h = geometry_.region.h;
    info.out_w = static_cast<std::uint16_t>(geometry_.out_w);
    info.out_h = static_cast<std::uint16_t>(geometry_.out_h);
    info.format = proto::FrameFormat::Etc2Rgb8;
    info.tile = static_cast<std::uint8_t>(geometry_.tile);
    info.raw_bytes = static_cast<std::uint32_t>(payload_.size());
    info.payload_bytes = static_cast<std::uint32_t>(n);
    info.tiles = sent;

    sink_(proto::encode(info));

    // Cut small on purpose: the chunk size is what bounds how long a pointer
    // message can sit behind a frame, because a queue cannot preempt bytes
    // that are already in the kernel buffer.
    for (std::size_t offset = 0; offset < n; offset += proto::kFrameChunkBytes) {
        const std::size_t take = std::min(proto::kFrameChunkBytes, n - offset);
        proto::FrameData chunk;
        chunk.seq = seq_;
        chunk.offset = static_cast<std::uint32_t>(offset);
        chunk.bytes.assign(reinterpret_cast<const std::byte*>(compressed_.data() + offset),
                           reinterpret_cast<const std::byte*>(compressed_.data() + offset + take));
        sink_(proto::encode(chunk));
    }

    // Only now, because a tile that never reached the queue is still owed.
    scheduler_.mark_sent(sent);

    ++stats_.frames;
    stats_.tiles += sent.size();
    stats_.raw_bytes += payload_.size();
    stats_.bytes += n;
    stats_.last_encode_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
}

void FrameSender::tick() {
    if (!streaming_ || !geometry_.valid() || !sink_) {
        return;
    }
    if (!ensure_source()) {
        return;
    }

    const bool holding = capture();

    // The readback rides on the frame arriving, not on the clock, because the
    // pixels can only be read while the frame is held and a still screen
    // produces no more frames. Gating it on the clock instead stranded dirt:
    // a change that the budget could not finish in one tick left the rest of
    // its tiles marked, and if the screen then stopped there was never
    // another frame to read them out of. They stayed wrong until something
    // else happened to move.
    //
    // One readback for the whole region, then tiles are cut out of it. This is
    // the expensive part and the one a compute shader replaces.
    if (holding && pixels_stale_ && scheduler_.anything_dirty()) {
        if (source_->read(geometry_.region, region_pixels_, region_stride_)) {
            region_valid_ = true;
            pixels_stale_ = false;
        }
    }
    if (!region_valid_) {
        return; // nothing has ever been read for this region
    }

    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::milliseconds(1000 / std::max<int>(request_.fps, 1));
    if (now - last_frame_ < interval) {
        return;
    }
    // A still screen costs nothing -- except the tile under the pen, which is
    // owed every batch whether or not it changed.
    if (!scheduler_.anything_dirty() && pen_tile() < 0) {
        return;
    }
    if (ready_ && !ready_()) {
        // The previous batch has not drained. Sending another would grow a
        // backlog of pictures that are each more out of date than the last.
        ++stats_.skipped_busy;
        return;
    }
    last_frame_ = now;

    // Sending is allowed without a frame in hand. The buffer is only used once
    // it holds everything that has been marked, so "no new frame" means "the
    // screen has not moved since this was read", which makes it current.
    send_batch();
}

} // namespace digitiz::host
