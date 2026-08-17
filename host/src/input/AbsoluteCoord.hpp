#pragma once

// The one piece of arithmetic that silently ruins a digitizer if it is off by
// one, so it lives here as a pure function and is verified exhaustively.
//
// SendInput's MOUSEEVENTF_ABSOLUTE coordinates are normalized to [0, 65535]
// across the virtual desktop. Windows converts back by *truncating*:
//
//     pixel = (n * extent) / 65536
//
// Therefore the forward direction needs ceiling division: pick the smallest n
// that still truncates to the intended pixel. Round-to-nearest lands a pixel
// short wherever the true quotient sits just under an integer, and the widely
// copied `n = v * 65535 / (extent - 1)` misses for the same reason. Measured
// with `digitiz_host --selftest`: ceiling gives 0.00 px error, the others 1 px.

#include <algorithm>
#include <cstdint>

namespace digitiz::host {

inline constexpr std::int64_t kNormalizedSpan = 65536;
inline constexpr std::int32_t kNormalizedMax = 65535;

// `offset` is a pixel offset from the virtual desktop origin, in [0, extent).
// Extents above kNormalizedSpan cannot round-trip — there would be more pixels
// than normalized values — but no real desktop is 65536 px across.
constexpr std::int32_t pixel_to_normalized(std::int64_t offset, std::int64_t extent) noexcept {
    if (extent <= 0) {
        return 0;
    }
    const std::int64_t clamped = std::clamp<std::int64_t>(offset, 0, extent - 1);
    const std::int64_t n = (clamped * kNormalizedSpan + extent - 1) / extent; // ceiling
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(n, 0, kNormalizedMax));
}

// Model of what Windows does with the value we hand it. Present so the test
// can assert the round-trip rather than trusting a comment.
constexpr std::int32_t normalized_to_pixel(std::int64_t normalized,
                                           std::int64_t extent) noexcept {
    if (extent <= 0) {
        return 0;
    }
    return static_cast<std::int32_t>((normalized * extent) / kNormalizedSpan);
}

} // namespace digitiz::host
