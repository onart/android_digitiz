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
        read_rect_ = core::Recti{};
        DZ_INFO("screen: %dx%d at (%d, %d) -> %dx%d, %d px tiles (%d), %u fps", fresh.region.w,
                fresh.region.h, fresh.region.x, fresh.region.y, fresh.out_w, fresh.out_h,
                fresh.tile, fresh.tile_count(), request.fps);
    }
}

void FrameSender::stop() {
    streaming_ = false;
    geometry_ready_ = false;
    read_rect_ = core::Recti{};
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
    } else {
        for (const core::Recti& dirty : update.dirty) {
            scheduler_.mark_dirty_rect(geometry_.tiles_touching(dirty));
        }
    }
    // Move rects would be handled here, if Windows ever produced any.
    for (const MoveRect& move : update.moves) {
        scheduler_.mark_dirty_rect(geometry_.tiles_touching(move.to));
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

    // Offsets are into what read_selection() actually read, which is the
    // bounding box of this batch rather than the whole region.
    const core::Recti& region = read_rect_;
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

bool FrameSender::read_selection() {
    core::Recti box{};
    bool any = false;
    for (const std::uint16_t index : selected_) {
        const core::Recti src = geometry_.tile_source_rect(static_cast<int>(index));
        if (src.w <= 0 || src.h <= 0) {
            continue;
        }
        if (!any) {
            box = src;
            any = true;
            continue;
        }
        const int x0 = std::min(box.x, src.x);
        const int y0 = std::min(box.y, src.y);
        const int x1 = std::max(box.x + box.w, src.x + src.w);
        const int y1 = std::max(box.y + box.h, src.y + src.h);
        box = core::Recti{x0, y0, x1 - x0, y1 - y0};
    }
    if (!any) {
        return false;
    }

    // Clipped here rather than left to the source, which would clip silently
    // and hand back a buffer that does not match the rectangle asked for --
    // and every tile would then be cut from the wrong place.
    const core::Recti bounds = source_->bounds();
    const int x0 = std::max(box.x, bounds.x);
    const int y0 = std::max(box.y, bounds.y);
    const int x1 = std::min(box.x + box.w, bounds.x + bounds.w);
    const int y1 = std::min(box.y + box.h, bounds.y + bounds.h);
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    box = core::Recti{x0, y0, x1 - x0, y1 - y0};

    const auto started = std::chrono::steady_clock::now();
    const bool ok = source_->read(box, region_pixels_, region_stride_);
    stats_.read_us += static_cast<std::uint64_t>(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
            .count());
    if (!ok) {
        return false;
    }
    read_rect_ = box;
    return true;
}

void FrameSender::update_recommendation(std::uint64_t batches, std::uint64_t busy) {
    if (batches < 4) {
        return; // not enough of a second to say anything about
    }

    if (busy > 0) {
        // Batches are not draining. That is the link or the phone, not this
        // side, and the only lever here is to ask for less of it. Backing off
        // faster than it climbs, because being behind costs a picture that is
        // out of date while being ahead only costs some idle link.
        recommended_ = std::max(budget_tiles_ * 2 / 3, 1);
        return;
    }

    // Only raise it on evidence. If the batches were not full, the budget was
    // not what limited them -- there was simply not that much to send -- and
    // what they cost says nothing about what a full one would cost. Holding
    // the last answer is better than inventing one, and on a still screen
    // there is no answer to give.
    const double per_batch =
        static_cast<double>(stats_.tiles - reported_tiles_) / static_cast<double>(batches);
    if (per_batch < budget_tiles_ * 0.9) {
        return;
    }

    // A quarter of the frame interval. The host draws its own window on this
    // thread and the pointer path shares the machine, so a batch that fills
    // the tick would be a batch that starves them.
    const double target_us = 1000000.0 / std::max<int>(request_.fps, 1) / 4.0;
    const double actual_us =
        static_cast<double>(stats_.encode_us - reported_encode_us_) / static_cast<double>(batches);
    if (actual_us <= 0.0) {
        return;
    }

    // Steered rather than solved: no model of what is fixed and what is per
    // tile, just the cost that was actually paid and which way to lean. It
    // converges over a few seconds and re-converges when the machine or the
    // picture changes, which a fitted constant would not.
    const double scale = std::clamp(target_us / actual_us, 0.5, 2.0);
    recommended_ = std::clamp(static_cast<int>(std::lround(budget_tiles_ * scale)), 1, 256);
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
    std::vector<std::uint16_t> sent;
    sent.reserve(selected_.size());

    // The capture encodes it itself where it can, and then nothing but blocks
    // crosses the bus. Where it cannot -- another platform, a tile hanging off
    // the edge of the output -- the pixels come across and are encoded here,
    // which is the same work in the same order and the same bytes out.
    jobs_.clear();
    jobs_.reserve(selected_.size());
    for (const std::uint16_t index : selected_) {
        const core::Recti out = geometry_.tile_output_rect(static_cast<int>(index));
        const core::Recti src = geometry_.tile_source_rect(static_cast<int>(index));
        if (out.w <= 0 || out.h <= 0 || src.w <= 0 || src.h <= 0) {
            jobs_.clear();
            break;
        }
        jobs_.push_back(TileJob{src, out.w, out.h});
    }

    if (!jobs_.empty() && source_->encode_tiles(jobs_, payload_)) {
        sent.assign(selected_.begin(), selected_.end());
        ++stats_.gpu_batches;
        stats_.read_us += static_cast<std::uint64_t>(source_->encode_dispatch_us() +
                                                     source_->encode_map_us());
    } else {
        // Choosing before reading is what makes the budget cheap. The readback
        // does not care how many tiles are wanted, only how large a box they
        // span, and one tile out of a full-screen region used to cost the
        // whole region.
        if (!read_selection()) {
            return;
        }

        std::vector<std::uint8_t> blocks;
        for (const std::uint16_t index : selected_) {
            int w = 0;
            int h = 0;
            const auto t0 = std::chrono::steady_clock::now();
            if (!gather_tile(static_cast<int>(index), w, h)) {
                continue;
            }
            const auto t1 = std::chrono::steady_clock::now();
            const bool encoded = etc2_encode(tile_pixels_.data(), w, h, w * 4, blocks);
            const auto t2 = std::chrono::steady_clock::now();
            stats_.gather_us += static_cast<std::uint64_t>(
                std::chrono::duration<double, std::micro>(t1 - t0).count());
            stats_.etc2_us += static_cast<std::uint64_t>(
                std::chrono::duration<double, std::micro>(t2 - t1).count());
            if (!encoded) {
                continue;
            }
            payload_.insert(payload_.end(), blocks.begin(), blocks.end());
            sent.push_back(index);
        }
    }
    if (sent.empty()) {
        return;
    }

    compressed_.resize(ZSTD_compressBound(payload_.size()));
    const auto zstd_started = std::chrono::steady_clock::now();
    const std::size_t n = ZSTD_compress(compressed_.data(), compressed_.size(), payload_.data(),
                                        payload_.size(), kZstdLevel);
    stats_.zstd_us += static_cast<std::uint64_t>(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - zstd_started)
            .count());
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
    stats_.encode_us += static_cast<std::uint64_t>(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
            .count());
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

    // Accumulates the dirty rects. Whether it acquired anything no longer
    // decides whether a batch can go out: the capture keeps its own copy of
    // the desktop, so the pixels are readable whether or not the screen moved.
    capture();

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
    send_batch();

    if (now - reported_ > std::chrono::seconds(1)) {
        const std::uint64_t batches = stats_.frames - reported_frames_;
        const std::uint64_t busy = stats_.skipped_busy - reported_busy_;
        if (reported_.time_since_epoch().count() != 0) {
            const double seconds = std::chrono::duration<double>(now - reported_).count();
            update_recommendation(batches, busy);
            DZ_DEBUG("screen: %.0f KiB/s on the wire, %.0f batch(es)/s, %.0f tile(s)/s, "
                     "%llu skipped while busy, budget %d, suggested %d",
                     (stats_.bytes - reported_bytes_) / 1024.0 / seconds,
                     batches / seconds, (stats_.tiles - reported_tiles_) / seconds,
                     static_cast<unsigned long long>(busy), budget_tiles_, recommended_);
        }
        reported_ = now;
        reported_bytes_ = stats_.bytes;
        reported_tiles_ = stats_.tiles;
        reported_frames_ = stats_.frames;
        reported_busy_ = stats_.skipped_busy;
        reported_encode_us_ = stats_.encode_us;
    }
}

} // namespace digitiz::host
