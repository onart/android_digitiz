#include "screen/CaptureProbe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <digitiz/core/log.hpp>

#include "screen/FrameSource.hpp"

namespace digitiz::host {

namespace {

// A 32-bit bottom-up BMP, which is the shortest path from BGRA to something a
// person can open. Only the probe writes these.
bool write_bmp(const std::string& path, const std::vector<std::uint8_t>& bgra, int w, int h,
               int stride) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }

    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(w) * h * 4;
    const std::uint32_t offset = 14 + 40;
    const std::uint32_t size = offset + pixel_bytes;

    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto i32 = [&](std::int32_t v) { std::fwrite(&v, 4, 1, f); };

    std::fwrite("BM", 1, 2, f);
    u32(size);
    u16(0);
    u16(0);
    u32(offset);

    u32(40);
    i32(w);
    i32(h); // positive: rows run bottom to top
    u16(1);
    u16(32);
    u32(0); // BI_RGB
    u32(pixel_bytes);
    i32(2835);
    i32(2835);
    u32(0);
    u32(0);

    for (int row = h - 1; row >= 0; --row) {
        std::fwrite(bgra.data() + static_cast<std::size_t>(row) * stride, 1,
                    static_cast<std::size_t>(w) * 4, f);
    }
    std::fclose(f);
    return true;
}

double area_of(const std::vector<core::Recti>& rects) {
    double total = 0.0;
    for (const core::Recti& r : rects) {
        total += static_cast<double>(r.w) * r.h;
    }
    return total;
}

} // namespace

bool run_capture_probe(int seconds, const std::string& bmp_path, CaptureProbeResult& out) {
    std::unique_ptr<IFrameSource> source = make_frame_source();
    if (!source) {
        DZ_ERROR("capture probe: no frame source on this platform");
        return false;
    }
    if (!source->start()) {
        return false;
    }

    const core::Recti bounds = source->bounds();
    const double screen_area = static_cast<double>(bounds.w) * bounds.h;
    DZ_INFO("capture probe: watching %dx%d for %d seconds -- use the PC normally, and scroll "
            "something",
            bounds.w, bounds.h, seconds);

    out = CaptureProbeResult{};
    double dirty_sum = 0.0;

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    auto last_frame_at = started;

    FrameUpdate update;
    std::vector<std::uint8_t> pixels;
    int stride = 0;
    bool have_pixels = false;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!source->next(update, 100)) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        out.interval_ms_mean += std::chrono::duration<double, std::milli>(now - last_frame_at).count();
        last_frame_at = now;

        ++out.frames;
        out.coalesced += update.accumulated > 1 ? update.accumulated - 1 : 0;
        out.dirty_rects += static_cast<int>(update.dirty.size());
        out.move_rects += static_cast<int>(update.moves.size());
        if (!update.moves.empty()) {
            ++out.frames_with_moves;
        }

        double fraction = 1.0;
        if (update.full) {
            ++out.full_frames;
        } else if (screen_area > 0.0) {
            // Overlapping rects would double count, but the answer only has to
            // be good enough to tell "a line of text" from "the whole screen".
            fraction = std::min(area_of(update.dirty) / screen_area, 1.0);
        }
        dirty_sum += fraction;
        out.dirty_fraction_max = std::max(out.dirty_fraction_max, fraction);

        // Read something every frame, so the cost of the readback is part of
        // what the interval measures rather than hidden by skipping it.
        if (source->read(bounds, pixels, stride)) {
            have_pixels = true;
        }
    }

    if (out.frames > 0) {
        out.dirty_fraction_mean = dirty_sum / out.frames;
        out.interval_ms_mean /= out.frames;
    }

    if (!bmp_path.empty() && have_pixels) {
        if (write_bmp(bmp_path, pixels, bounds.w, bounds.h, stride)) {
            DZ_INFO("capture probe: wrote %s", bmp_path.c_str());
        } else {
            DZ_WARN("capture probe: could not write %s", bmp_path.c_str());
        }
    }

    source->stop();

    DZ_INFO("capture probe: %d frames, %.1f ms apart on average, %u coalesced", out.frames,
            out.interval_ms_mean, out.coalesced);
    DZ_INFO("capture probe: dirty %d rect(s) total, %.1f%% of the screen on average, %.1f%% worst",
            out.dirty_rects, out.dirty_fraction_mean * 100.0, out.dirty_fraction_max * 100.0);
    DZ_INFO("capture probe: %d move rect(s) across %d frame(s); %d full frame(s)", out.move_rects,
            out.frames_with_moves, out.full_frames);
    return true;
}

} // namespace digitiz::host
