#include <doctest/doctest.h>

#include "input/AbsoluteCoord.hpp"

using namespace digitiz::host;

namespace {

// Real and awkward desktop extents, including odd numbers and multi-monitor
// widths, plus the degenerate ends.
constexpr std::int32_t kExtents[] = {
    1, 2, 3, 640, 800, 1079, 1080, 1200, 1366, 1440, 1920,
    2160, 2560, 3440, 3840, 5120, 5760, 7680, 11520, 32768, 65536,
};

} // namespace

TEST_CASE("every pixel survives the round-trip through Windows' truncation") {
    for (const std::int32_t extent : kExtents) {
        CAPTURE(extent);
        for (std::int32_t px = 0; px < extent; ++px) {
            const std::int32_t n = pixel_to_normalized(px, extent);
            const std::int32_t back = normalized_to_pixel(n, extent);
            if (back != px) {
                CAPTURE(px);
                CAPTURE(n);
                CAPTURE(back);
                REQUIRE(back == px); // fails loudly with the offending pixel
            }
        }
    }
}

TEST_CASE("normalized values stay inside the wire range") {
    for (const std::int32_t extent : kExtents) {
        CAPTURE(extent);
        for (std::int32_t px = 0; px < extent; ++px) {
            const std::int32_t n = pixel_to_normalized(px, extent);
            REQUIRE(n >= 0);
            REQUIRE(n <= kNormalizedMax);
        }
    }
}

TEST_CASE("the mapping is monotonic — a rightward move never goes left") {
    for (const std::int32_t extent : kExtents) {
        CAPTURE(extent);
        std::int32_t prev = -1;
        for (std::int32_t px = 0; px < extent; ++px) {
            const std::int32_t n = pixel_to_normalized(px, extent);
            REQUIRE(n > prev);
            prev = n;
        }
    }
}

TEST_CASE("edges land exactly on the edges") {
    for (const std::int32_t extent : kExtents) {
        CAPTURE(extent);
        CHECK(pixel_to_normalized(0, extent) == 0);
        CHECK(normalized_to_pixel(pixel_to_normalized(extent - 1, extent), extent) == extent - 1);
    }
}

TEST_CASE("out-of-range offsets clamp instead of wrapping") {
    constexpr std::int32_t extent = 1920;

    CHECK(pixel_to_normalized(-1, extent) == pixel_to_normalized(0, extent));
    CHECK(pixel_to_normalized(-100000, extent) == 0);
    CHECK(pixel_to_normalized(extent, extent) == pixel_to_normalized(extent - 1, extent));
    CHECK(pixel_to_normalized(999999, extent) == pixel_to_normalized(extent - 1, extent));
}

TEST_CASE("a degenerate extent does not divide by zero") {
    CHECK(pixel_to_normalized(5, 0) == 0);
    CHECK(pixel_to_normalized(5, -10) == 0);
    CHECK(normalized_to_pixel(1000, 0) == 0);
}

TEST_CASE("round-to-nearest would have been wrong — the bug this guards") {
    // The exact sample --selftest caught on a 1920x1080 desktop.
    constexpr std::int64_t extent = 1080;
    constexpr std::int64_t px = 809;

    const std::int64_t nearest = (px * kNormalizedSpan + extent / 2) / extent;
    CHECK(normalized_to_pixel(nearest, extent) == 808); // one pixel short

    CHECK(normalized_to_pixel(pixel_to_normalized(px, extent), extent) == 809);
}
