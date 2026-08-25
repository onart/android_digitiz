#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

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

Image make(int w, int h) {
    Image im;
    im.w = w;
    im.h = h;
    im.bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
    return im;
}

Image flat(int w, int h, int r, int g, int b) {
    Image im = make(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            im.set(x, y, r, g, b);
        }
    }
    return im;
}

// Black text on white is what a desktop mostly is, and it is pure luminance
// variation, which is the shape ETC1's model was built around.
Image text_like(int w, int h) {
    Image im = flat(w, h, 255, 255, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool stroke = (x % 7) < 2 && (y % 11) < 8;
            const bool edge = (x % 7) == 2 && (y % 11) < 8;
            if (stroke) {
                im.set(x, y, 20, 20, 20);
            } else if (edge) {
                im.set(x, y, 140, 140, 140); // the antialiased edge
            }
        }
    }
    return im;
}

Image gradient(int w, int h) {
    Image im = make(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            im.set(x, y, x * 255 / std::max(w - 1, 1), y * 255 / std::max(h - 1, 1), 128);
        }
    }
    return im;
}

double psnr(const Image& a, const std::vector<std::uint8_t>& b) {
    REQUIRE(a.bgra.size() == b.size());
    double sum = 0.0;
    std::size_t samples = 0;
    for (std::size_t i = 0; i < a.bgra.size(); i += 4) {
        for (int c = 0; c < 3; ++c) { // alpha is not carried by RGB8
            const double d = static_cast<double>(a.bgra[i + c]) - b[i + c];
            sum += d * d;
            ++samples;
        }
    }
    if (sum == 0.0) {
        return 1000.0;
    }
    const double mse = sum / static_cast<double>(samples);
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

double round_trip_psnr(const Image& im) {
    std::vector<std::uint8_t> blocks;
    REQUIRE(etc2_encode(im.bgra.data(), im.w, im.h, im.stride(), blocks));
    CHECK(blocks.size() == etc2_size(im.w, im.h));

    std::vector<std::uint8_t> back;
    REQUIRE(etc2_decode(blocks.data(), blocks.size(), im.w, im.h, back));
    return psnr(im, back);
}

} // namespace

TEST_CASE("the ratio is fixed, which is what the tile budget is built on") {
    // Four bits a pixel, whatever the picture is: a batch of tiles has a size
    // that is known before anything is compressed.
    CHECK(etc2_size(4, 4) == 8);
    CHECK(etc2_size(64, 64) == 2048);
    CHECK(etc2_size(64, 64) * 8 == static_cast<std::size_t>(64) * 64 * 4);
    CHECK(etc2_size(0, 0) == 0);
}

TEST_CASE("dimensions have to be whole blocks") {
    const Image im = flat(8, 8, 10, 20, 30);
    std::vector<std::uint8_t> blocks;
    CHECK_FALSE(etc2_encode(im.bgra.data(), 6, 8, im.stride(), blocks));
    CHECK_FALSE(etc2_encode(im.bgra.data(), 8, 6, im.stride(), blocks));
    CHECK_FALSE(etc2_encode(nullptr, 8, 8, 32, blocks));
}

TEST_CASE("a flat fill comes back almost exactly") {
    // The smallest modifier is 2, so a flat block cannot be exact -- but two
    // levels out of 255 is what "almost" means here, and most of a desktop is
    // flat fill.
    for (const auto& c : {std::array<int, 3>{255, 255, 255}, std::array<int, 3>{0, 0, 0},
                          std::array<int, 3>{32, 64, 128}, std::array<int, 3>{200, 30, 90}}) {
        const Image im = flat(16, 16, c[0], c[1], c[2]);
        const double q = round_trip_psnr(im);
        CHECK_MESSAGE(q > 38.0, "flat fill psnr ", q);
    }
}

TEST_CASE("black on white text survives, which is what a desktop mostly is") {
    const double q = round_trip_psnr(text_like(64, 64));
    CHECK_MESSAGE(q > 25.0, "text psnr ", q);
}

TEST_CASE("a smooth gradient survives") {
    const double q = round_trip_psnr(gradient(64, 64));
    CHECK_MESSAGE(q > 30.0, "gradient psnr ", q);
}

TEST_CASE("noise is the worst case and is still not garbage") {
    Image im = make(32, 32);
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 255);
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            im.set(x, y, dist(rng), dist(rng), dist(rng));
        }
    }
    // Nothing can do much with random chroma inside a 4x4; this only has to
    // stay recognisable rather than fall apart.
    const double q = round_trip_psnr(im);
    CHECK_MESSAGE(q > 10.0, "noise psnr ", q);
}

TEST_CASE("both halves of a split block keep their own colour") {
    // The left half red, the right half blue: a block that only survives if
    // the sub-block split and the flip bit are being used.
    Image im = make(4, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            im.set(x, y, x < 2 ? 200 : 20, 20, x < 2 ? 20 : 200);
        }
    }
    std::vector<std::uint8_t> blocks;
    REQUIRE(etc2_encode(im.bgra.data(), 4, 4, im.stride(), blocks));
    std::vector<std::uint8_t> back;
    REQUIRE(etc2_decode(blocks.data(), blocks.size(), 4, 4, back));

    const auto at = [&](int x, int y, int c) {
        return static_cast<int>(back[(static_cast<std::size_t>(y) * 4 + x) * 4 + c]);
    };
    // Red is channel 2 and blue channel 0 in BGRA.
    CHECK(at(0, 0, 2) > at(0, 0, 0));
    CHECK(at(3, 0, 0) > at(3, 0, 2));
}

TEST_CASE("a horizontal split is found too, not just a vertical one") {
    Image im = make(4, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            im.set(x, y, y < 2 ? 220 : 30, y < 2 ? 220 : 30, y < 2 ? 220 : 30);
        }
    }
    std::vector<std::uint8_t> blocks;
    REQUIRE(etc2_encode(im.bgra.data(), 4, 4, im.stride(), blocks));
    // Bit 32 of the big-endian block is the flip bit: byte 3, bit 0.
    CHECK((blocks[3] & 1) == 1);

    std::vector<std::uint8_t> back;
    REQUIRE(etc2_decode(blocks.data(), blocks.size(), 4, 4, back));
    CHECK(back[(0 * 4 + 0) * 4 + 1] > 180);
    CHECK(back[(3 * 4 + 0) * 4 + 1] < 70);
}

TEST_CASE("encoding is deterministic, so an unchanged tile is byte-identical") {
    // Re-sending a tile whose pixels did not change must produce the same
    // bytes, or a dirty-rect scheme would be re-sending noise.
    const Image im = text_like(32, 32);
    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    REQUIRE(etc2_encode(im.bgra.data(), im.w, im.h, im.stride(), a));
    REQUIRE(etc2_encode(im.bgra.data(), im.w, im.h, im.stride(), b));
    CHECK(a == b);
}

TEST_CASE("a tile encodes the same whether it is alone or inside a bigger image") {
    // Tiles are encoded independently and land in a texture next to each
    // other, so a block must not depend on what is beside it.
    const Image big = text_like(32, 32);
    std::vector<std::uint8_t> whole;
    REQUIRE(etc2_encode(big.bgra.data(), big.w, big.h, big.stride(), whole));

    // The 8x8 patch at (8, 8), encoded on its own from the same rows.
    const std::uint8_t* patch = big.bgra.data() + (8 * static_cast<std::size_t>(big.w) + 8) * 4;
    std::vector<std::uint8_t> alone;
    REQUIRE(etc2_encode(patch, 8, 8, big.stride(), alone));

    for (int by = 0; by < 2; ++by) {
        for (int bx = 0; bx < 2; ++bx) {
            const std::size_t from_whole =
                (static_cast<std::size_t>(2 + by) * (big.w / 4) + (2 + bx)) * kEtc2BlockBytes;
            const std::size_t from_alone =
                (static_cast<std::size_t>(by) * 2 + bx) * kEtc2BlockBytes;
            for (std::size_t i = 0; i < kEtc2BlockBytes; ++i) {
                CHECK(whole[from_whole + i] == alone[from_alone + i]);
            }
        }
    }
}

TEST_CASE("decoding refuses a buffer that is too short to hold the blocks") {
    const Image im = flat(16, 16, 10, 10, 10);
    std::vector<std::uint8_t> blocks;
    REQUIRE(etc2_encode(im.bgra.data(), im.w, im.h, im.stride(), blocks));

    std::vector<std::uint8_t> back;
    CHECK_FALSE(etc2_decode(blocks.data(), blocks.size() - 1, 16, 16, back));
    CHECK_FALSE(etc2_decode(nullptr, blocks.size(), 16, 16, back));
}
