#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <zstd.h>

#include "screen/Etc2.hpp"

using namespace digitiz::host;

namespace {

struct Image {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> bgra;
    int stride() const { return w * 4; }
    void set(int x, int y, int r, int g, int b) {
        std::uint8_t* p = bgra.data() + (static_cast<std::size_t>(y) * w + x) * 4;
        p[0] = static_cast<std::uint8_t>(b);
        p[1] = static_cast<std::uint8_t>(g);
        p[2] = static_cast<std::uint8_t>(r);
        p[3] = 255;
    }
};

Image blank(int w, int h) {
    Image im;
    im.w = w;
    im.h = h;
    im.bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
    return im;
}

// Roughly what a window looks like: a big flat body, a toolbar strip, and
// lines of text. The interesting property for zstd is that most of it is the
// same few block patterns over and over.
Image window_like(int w, int h) {
    Image im = blank(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (y < h / 12) {
                im.set(x, y, 240, 240, 242); // title bar
            } else if (y < h / 6) {
                im.set(x, y, 224, 226, 230); // toolbar
            } else {
                const bool glyph = (y % 14) < 9 && ((x / 3 + y) % 11) < 2 && x < w * 3 / 4;
                im.set(x, y, glyph ? 25 : 255, glyph ? 25 : 255, glyph ? 28 : 255);
            }
        }
    }
    return im;
}

// The floor. Random pixels make random blocks, and random blocks are exactly
// what zstd cannot do anything with -- which is the point: this measures what
// the budget can rely on, not what it usually gets. An arithmetic pattern
// would look photographic and compress like wallpaper, flattering the number
// into meaninglessness.
Image noise(int w, int h) {
    Image im = blank(w, h);
    std::mt19937 rng(20260825);
    std::uniform_int_distribution<int> dist(0, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            im.set(x, y, dist(rng), dist(rng), dist(rng));
        }
    }
    return im;
}

struct Packed {
    std::size_t etc2 = 0;
    std::size_t compressed = 0;
    double ratio_from_raw = 0.0;
};

Packed pack(const Image& im) {
    std::vector<std::uint8_t> blocks;
    REQUIRE(etc2_encode(im.bgra.data(), im.w, im.h, im.stride(), blocks));

    std::vector<std::uint8_t> squeezed(ZSTD_compressBound(blocks.size()));
    // Level 1: this runs per frame on the same machine that is injecting
    // pointer events, so throughput matters more than the last few percent.
    const std::size_t n =
        ZSTD_compress(squeezed.data(), squeezed.size(), blocks.data(), blocks.size(), 1);
    REQUIRE_FALSE(ZSTD_isError(n));
    squeezed.resize(n);

    // Straight back out again, because a payload that cannot be unpacked is
    // not a small payload.
    std::vector<std::uint8_t> back(blocks.size());
    const std::size_t out =
        ZSTD_decompress(back.data(), back.size(), squeezed.data(), squeezed.size());
    REQUIRE_FALSE(ZSTD_isError(out));
    CHECK(out == blocks.size());
    CHECK(back == blocks);

    Packed p;
    p.etc2 = blocks.size();
    p.compressed = n;
    p.ratio_from_raw = static_cast<double>(im.bgra.size()) / static_cast<double>(n);
    return p;
}

} // namespace

TEST_CASE("a window-like tile packs far past the fixed 8:1") {
    const Packed p = pack(window_like(256, 256));
    // Flat fills and repeated glyph shapes become the same blocks over and
    // over, which is the redundancy ETC2 leaves behind and zstd collects.
    CHECK_MESSAGE(p.ratio_from_raw > 20.0, "window ratio ", p.ratio_from_raw);
}

TEST_CASE("the worst content still cannot cost more than the fixed ratio") {
    const Packed p = pack(noise(256, 256));
    // Whatever is on screen, a tile costs about its block size and no more.
    // That floor is what makes a tile budget a budget.
    CHECK_MESSAGE(p.ratio_from_raw > 7.0, "noise ratio ", p.ratio_from_raw);
    CHECK(p.compressed <= p.etc2 + 128); // zstd never has to inflate it much
}

TEST_CASE("a flat tile costs almost nothing") {
    Image im = blank(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            im.set(x, y, 255, 255, 255);
        }
    }
    const Packed p = pack(im);
    // An untouched region of a window should not be paying for itself.
    CHECK_MESSAGE(p.compressed < 128, "flat bytes ", p.compressed);
}

TEST_CASE("the uncompressed size is exactly what the tile budget assumes") {
    // The scheduler hands out a number of tiles, and the sender has to know
    // what that costs before compressing anything.
    CHECK(etc2_size(64, 64) == 2048);
    const Packed p = pack(window_like(64, 64));
    CHECK(p.etc2 == etc2_size(64, 64));
}
