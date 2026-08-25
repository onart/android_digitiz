// Runs the whole send path with no phone attached, and reassembles what came
// out of it.
//
// Every piece has its own tests, which is not the same as the pieces fitting
// together: the geometry, the scheduler, the encoder and the chunking all
// agree with their own specifications and could still assemble into a picture
// with the tiles in the wrong places. So this decodes the batches back into an
// image and writes it out, which is the first thing that can be wrong in a way
// only an eye will catch.
//
// It is also, deliberately, the reference for what the guest has to do.

#include "screen/FrameProbe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include <zstd.h>

#include <digitiz/core/log.hpp>
#include <digitiz/proto/framer.hpp>
#include <digitiz/proto/messages.hpp>

#include "display/DisplayInfo.hpp"
#include "screen/Etc2.hpp"
#include "screen/FrameSender.hpp"
#include "screen/ViewportGeometry.hpp"

namespace digitiz::host {

namespace {

bool write_bmp(const std::string& path, const std::vector<std::uint8_t>& bgra, int w, int h) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(w) * h * 4;
    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto i32 = [&](std::int32_t v) { std::fwrite(&v, 4, 1, f); };

    std::fwrite("BM", 1, 2, f);
    u32(54 + pixel_bytes);
    u16(0);
    u16(0);
    u32(54);
    u32(40);
    i32(w);
    i32(h);
    u16(1);
    u16(32);
    u32(0);
    u32(pixel_bytes);
    i32(2835);
    i32(2835);
    u32(0);
    u32(0);
    for (int row = h - 1; row >= 0; --row) {
        std::fwrite(bgra.data() + static_cast<std::size_t>(row) * w * 4, 1,
                    static_cast<std::size_t>(w) * 4, f);
    }
    std::fclose(f);
    return true;
}

// What the guest will have to do: hold the surface, and paint tiles into it as
// batches arrive. Nothing here is a picture on its own.
class Receiver {
public:
    void on_message(proto::MsgType type, std::span<const std::byte> payload) {
        if (type == proto::MsgType::FrameInfo) {
            proto::FrameInfo info;
            if (!proto::decode(payload, info)) {
                ++malformed;
                return;
            }
            begin(info);
        } else if (type == proto::MsgType::FrameData) {
            proto::FrameData chunk;
            if (!proto::decode(payload, chunk)) {
                ++malformed;
                return;
            }
            append(chunk);
        }
    }

    int batches = 0;
    int tiles = 0;
    int malformed = 0;
    int mismatched_seq = 0;
    int surface_w = 0;
    int surface_h = 0;
    std::vector<std::uint8_t> surface;

private:
    void begin(const proto::FrameInfo& info) {
        info_ = info;
        compressed_.assign(info.payload_bytes, 0);
        received_ = 0;
        have_info_ = true;

        if (surface_w != info.out_w || surface_h != info.out_h) {
            surface_w = info.out_w;
            surface_h = info.out_h;
            // Deliberately not black: anything that stays this colour is a
            // tile that never arrived, which is exactly what wants to be
            // visible rather than plausible.
            surface.assign(static_cast<std::size_t>(surface_w) * surface_h * 4, 0x60);
        }
    }

    void append(const proto::FrameData& chunk) {
        if (!have_info_) {
            return;
        }
        if (chunk.seq != info_.seq) {
            ++mismatched_seq;
            return;
        }
        if (chunk.offset + chunk.bytes.size() > compressed_.size()) {
            ++malformed;
            return;
        }
        std::memcpy(compressed_.data() + chunk.offset, chunk.bytes.data(), chunk.bytes.size());
        received_ += chunk.bytes.size();
        if (received_ >= compressed_.size()) {
            finish();
        }
    }

    void finish() {
        have_info_ = false;

        std::vector<std::uint8_t> blocks(info_.raw_bytes);
        const std::size_t n = ZSTD_decompress(blocks.data(), blocks.size(), compressed_.data(),
                                              compressed_.size());
        if (ZSTD_isError(n) || n != blocks.size()) {
            ++malformed;
            return;
        }

        const ViewportGeometry g =
            make_geometry(core::Recti{info_.x, info_.y, info_.w, info_.h}, info_.out_w,
                          info_.out_h, info_.tile);

        std::size_t offset = 0;
        std::vector<std::uint8_t> pixels;
        for (const std::uint16_t index : info_.tiles) {
            const core::Recti rect = g.tile_output_rect(static_cast<int>(index));
            if (rect.w <= 0 || rect.h <= 0) {
                ++malformed;
                return;
            }
            const std::size_t size = etc2_size(rect.w, rect.h);
            if (offset + size > blocks.size()) {
                ++malformed;
                return;
            }
            if (!etc2_decode(blocks.data() + offset, size, rect.w, rect.h, pixels)) {
                ++malformed;
                return;
            }
            offset += size;

            for (int y = 0; y < rect.h; ++y) {
                std::memcpy(surface.data() +
                                (static_cast<std::size_t>(rect.y + y) * surface_w + rect.x) * 4,
                            pixels.data() + static_cast<std::size_t>(y) * rect.w * 4,
                            static_cast<std::size_t>(rect.w) * 4);
            }
            ++tiles;
        }
        // Every byte accounted for: a leftover means the two sides disagree
        // about how big a tile is.
        if (offset != blocks.size()) {
            ++malformed;
        }
        ++batches;
    }

    proto::FrameInfo info_;
    std::vector<std::uint8_t> compressed_;
    std::size_t received_ = 0;
    bool have_info_ = false;
};

} // namespace

bool run_frame_probe(int seconds, int divisor, const std::string& bmp_path) {
    std::unique_ptr<IDisplayInfo> display = make_display_info();
    if (!display) {
        DZ_ERROR("frame probe: no display backend");
        return false;
    }
    const core::Recti desktop = display->query().virtual_bounds;

    FrameSender sender;
    proto::Framer framer;
    Receiver receiver;

    // The messages go straight into a framer, so the probe reads them exactly
    // as the guest will: as a byte stream that has to be re-split.
    sender.set_sink(
        [&](std::vector<std::byte> message) {
            framer.push(message);
            framer.drain([&](proto::MsgType type, std::uint8_t /*flags*/,
                             std::span<const std::byte> payload) {
                receiver.on_message(type, payload);
            });
        },
        [] { return true; }); // no socket here, so a batch always counts as drained

    proto::ViewportReq request;
    request.x = desktop.x;
    request.y = desktop.y;
    request.w = desktop.w;
    request.h = desktop.h;
    request.out_w = static_cast<std::uint16_t>(desktop.w / std::max(divisor, 1));
    request.out_h = static_cast<std::uint16_t>(desktop.h / std::max(divisor, 1));
    request.fps = 20;
    request.format = proto::FrameFormat::Etc2Rgb8;
    request.tile = 64;
    sender.set_viewport(request);

    if (!sender.streaming()) {
        DZ_ERROR("frame probe: the sender refused the request");
        return false;
    }
    DZ_INFO("frame probe: %d seconds, %dx%d -> %dx%d -- use the PC normally", seconds, desktop.w,
            desktop.h, sender.geometry().out_w, sender.geometry().out_h);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        sender.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    const FrameSender::Stats& s = sender.stats();
    DZ_INFO("frame probe: %llu batch(es), %llu tile(s), %.1f KiB sent from %.1f KiB of blocks "
            "(%.1fx)",
            static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.tiles),
            s.bytes / 1024.0, s.raw_bytes / 1024.0,
            s.bytes > 0 ? static_cast<double>(s.raw_bytes) / s.bytes : 0.0);
    DZ_INFO("frame probe: %d tile(s) reassembled across %d batch(es); %d malformed, %d stray seq",
            receiver.tiles, receiver.batches, receiver.malformed, receiver.mismatched_seq);
    DZ_INFO("frame probe: last encode %.1f ms, %llu capture(s)", s.last_encode_ms,
            static_cast<unsigned long long>(s.captures));

    sender.stop();

    if (receiver.surface.empty()) {
        DZ_ERROR("frame probe: nothing was reassembled");
        return false;
    }
    if (!bmp_path.empty()) {
        if (write_bmp(bmp_path, receiver.surface, receiver.surface_w, receiver.surface_h)) {
            DZ_INFO("frame probe: wrote %s", bmp_path.c_str());
        }
    }
    return receiver.malformed == 0 && receiver.batches > 0;
}

} // namespace digitiz::host
