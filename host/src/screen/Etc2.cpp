#include "screen/Etc2.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace digitiz::host {

namespace {

// Intensity modifier sets. Index 0 is a small step up, 1 a large step up, and
// 2 and 3 the same two downwards -- the order matters, and getting it wrong
// produces a picture that is merely a bit odd rather than obviously broken,
// which is why the decoder below is tested against the encoder.
constexpr std::array<std::array<int, 4>, 8> kModifiers{{
    {2, 8, -2, -8},
    {5, 17, -5, -17},
    {9, 29, -9, -29},
    {13, 42, -13, -42},
    {18, 60, -18, -60},
    {24, 80, -24, -80},
    {33, 106, -33, -106},
    {47, 183, -47, -183},
}};

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

int clamp255(int v) noexcept {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// Four bits per channel, expanded by replication.
int quant4(int v) noexcept {
    return std::clamp((v * 15 + 127) / 255, 0, 15);
}
int expand4(int q) noexcept {
    return q * 17;
}

// Five bits per channel, expanded by taking the top three bits again.
int quant5(int v) noexcept {
    return std::clamp((v * 31 + 127) / 255, 0, 31);
}
int expand5(int q) noexcept {
    return (q << 3) | (q >> 2);
}

// Which pixels of the 4x4 belong to sub-block `half`. flip 0 splits it into
// left and right halves, flip 1 into top and bottom.
void subblock_pixels(int flip, int half, std::array<int, 8>& out) noexcept {
    int n = 0;
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            const bool mine = flip == 0 ? (x >= 2) == (half == 1) : (y >= 2) == (half == 1);
            if (mine) {
                out[static_cast<std::size_t>(n++)] = x * 4 + y; // column-major, as ETC numbers them
            }
        }
    }
}

// Best modifier table and per-pixel indices for one sub-block against one base
// colour, and the squared error that costs.
int fit_subblock(const std::array<Rgb, 8>& pixels, Rgb base, int& table_out,
                 std::array<int, 8>& indices_out) noexcept {
    int best_error = -1;
    for (int table = 0; table < 8; ++table) {
        int error = 0;
        std::array<int, 8> indices{};
        for (int i = 0; i < 8; ++i) {
            int best_pixel = -1;
            int best_index = 0;
            for (int index = 0; index < 4; ++index) {
                const int m = kModifiers[static_cast<std::size_t>(table)]
                                        [static_cast<std::size_t>(index)];
                const int dr = clamp255(base.r + m) - pixels[static_cast<std::size_t>(i)].r;
                const int dg = clamp255(base.g + m) - pixels[static_cast<std::size_t>(i)].g;
                const int db = clamp255(base.b + m) - pixels[static_cast<std::size_t>(i)].b;
                const int e = dr * dr + dg * dg + db * db;
                if (best_pixel < 0 || e < best_pixel) {
                    best_pixel = e;
                    best_index = index;
                }
            }
            indices[static_cast<std::size_t>(i)] = best_index;
            error += best_pixel;
            if (best_error >= 0 && error >= best_error) {
                break; // this table is already worse than one already found
            }
        }
        if (best_error < 0 || error < best_error) {
            best_error = error;
            table_out = table;
            indices_out = indices;
        }
    }
    return best_error;
}

struct Candidate {
    int error = -1;
    int flip = 0;
    bool differential = false;
    // Both halves, already expanded to eight bits for fitting, plus the raw
    // quantized values the bit packing needs.
    std::array<int, 3> q0{};
    std::array<int, 3> q1{}; // individual: 4-bit each. differential: deltas.
    std::array<int, 2> table{};
    std::array<std::array<int, 8>, 2> indices{};
};

void write_block(const Candidate& c, std::uint8_t* out) noexcept {
    std::uint64_t bits = 0;

    if (c.differential) {
        bits |= static_cast<std::uint64_t>(c.q0[0] & 0x1F) << 59;
        bits |= static_cast<std::uint64_t>(c.q1[0] & 0x07) << 56;
        bits |= static_cast<std::uint64_t>(c.q0[1] & 0x1F) << 51;
        bits |= static_cast<std::uint64_t>(c.q1[1] & 0x07) << 48;
        bits |= static_cast<std::uint64_t>(c.q0[2] & 0x1F) << 43;
        bits |= static_cast<std::uint64_t>(c.q1[2] & 0x07) << 40;
        bits |= std::uint64_t{1} << 33;
    } else {
        bits |= static_cast<std::uint64_t>(c.q0[0] & 0x0F) << 60;
        bits |= static_cast<std::uint64_t>(c.q1[0] & 0x0F) << 56;
        bits |= static_cast<std::uint64_t>(c.q0[1] & 0x0F) << 52;
        bits |= static_cast<std::uint64_t>(c.q1[1] & 0x0F) << 48;
        bits |= static_cast<std::uint64_t>(c.q0[2] & 0x0F) << 44;
        bits |= static_cast<std::uint64_t>(c.q1[2] & 0x0F) << 40;
    }

    bits |= static_cast<std::uint64_t>(c.table[0] & 0x07) << 37;
    bits |= static_cast<std::uint64_t>(c.table[1] & 0x07) << 34;
    bits |= static_cast<std::uint64_t>(c.flip & 1) << 32;

    std::array<int, 8> pixels{};
    for (int half = 0; half < 2; ++half) {
        subblock_pixels(c.flip, half, pixels);
        for (int i = 0; i < 8; ++i) {
            const int pixel = pixels[static_cast<std::size_t>(i)];
            const int index = c.indices[static_cast<std::size_t>(half)][static_cast<std::size_t>(i)];
            bits |= static_cast<std::uint64_t>((index >> 1) & 1) << (16 + pixel);
            bits |= static_cast<std::uint64_t>(index & 1) << pixel;
        }
    }

    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>((bits >> (56 - i * 8)) & 0xFF);
    }
}

} // namespace

bool etc2_encode(const std::uint8_t* bgra, int w, int h, int stride,
                 std::vector<std::uint8_t>& out) {
    if (bgra == nullptr || w <= 0 || h <= 0 || w % 4 != 0 || h % 4 != 0) {
        return false;
    }
    out.resize(etc2_size(w, h));

    const int cols = w / 4;
    const int rows = h / 4;

    for (int by = 0; by < rows; ++by) {
        for (int bx = 0; bx < cols; ++bx) {
            std::array<Rgb, 16> block{};
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const std::uint8_t* p =
                        bgra + static_cast<std::size_t>(by * 4 + y) * stride +
                        static_cast<std::size_t>(bx * 4 + x) * 4;
                    block[static_cast<std::size_t>(x * 4 + y)] = Rgb{p[2], p[1], p[0]};
                }
            }

            Candidate best;
            for (int flip = 0; flip < 2; ++flip) {
                std::array<std::array<Rgb, 8>, 2> halves{};
                std::array<Rgb, 2> average{};
                std::array<int, 8> pixels{};

                for (int half = 0; half < 2; ++half) {
                    subblock_pixels(flip, half, pixels);
                    int sr = 0;
                    int sg = 0;
                    int sb = 0;
                    for (int i = 0; i < 8; ++i) {
                        const Rgb c = block[static_cast<std::size_t>(pixels[static_cast<std::size_t>(i)])];
                        halves[static_cast<std::size_t>(half)][static_cast<std::size_t>(i)] = c;
                        sr += c.r;
                        sg += c.g;
                        sb += c.b;
                    }
                    average[static_cast<std::size_t>(half)] = Rgb{(sr + 4) / 8, (sg + 4) / 8,
                                                                  (sb + 4) / 8};
                }

                // Individual: four bits a channel, the two halves unrelated.
                {
                    Candidate c;
                    c.flip = flip;
                    c.differential = false;
                    int error = 0;
                    for (int half = 0; half < 2; ++half) {
                        const Rgb avg = average[static_cast<std::size_t>(half)];
                        const int qr = quant4(avg.r);
                        const int qg = quant4(avg.g);
                        const int qb = quant4(avg.b);
                        const Rgb base{expand4(qr), expand4(qg), expand4(qb)};
                        (half == 0 ? c.q0 : c.q1) = {qr, qg, qb};
                        error += fit_subblock(halves[static_cast<std::size_t>(half)], base,
                                              c.table[static_cast<std::size_t>(half)],
                                              c.indices[static_cast<std::size_t>(half)]);
                    }
                    c.error = error;
                    if (best.error < 0 || c.error < best.error) {
                        best = c;
                    }
                }

                // Differential: five bits for the first half and a three-bit
                // signed step for the second, which is finer when the two
                // halves are near each other and impossible when they are not.
                // Clamping the step rather than giving up keeps this mode
                // always available; if the clamp hurts, the individual
                // candidate above wins on error anyway.
                {
                    Candidate c;
                    c.flip = flip;
                    c.differential = true;
                    std::array<int, 3> base_q{quant5(average[0].r), quant5(average[0].g),
                                              quant5(average[0].b)};
                    const std::array<int, 3> want{quant5(average[1].r), quant5(average[1].g),
                                                  quant5(average[1].b)};
                    std::array<int, 3> delta{};
                    std::array<int, 3> second{};
                    for (int i = 0; i < 3; ++i) {
                        delta[static_cast<std::size_t>(i)] =
                            std::clamp(want[static_cast<std::size_t>(i)] -
                                           base_q[static_cast<std::size_t>(i)],
                                       -4, 3);
                        second[static_cast<std::size_t>(i)] =
                            std::clamp(base_q[static_cast<std::size_t>(i)] +
                                           delta[static_cast<std::size_t>(i)],
                                       0, 31);
                        delta[static_cast<std::size_t>(i)] =
                            second[static_cast<std::size_t>(i)] - base_q[static_cast<std::size_t>(i)];
                    }

                    c.q0 = base_q;
                    c.q1 = delta;
                    const Rgb base0{expand5(base_q[0]), expand5(base_q[1]), expand5(base_q[2])};
                    const Rgb base1{expand5(second[0]), expand5(second[1]), expand5(second[2])};
                    int error = fit_subblock(halves[0], base0, c.table[0], c.indices[0]);
                    error += fit_subblock(halves[1], base1, c.table[1], c.indices[1]);
                    c.error = error;
                    if (best.error < 0 || c.error < best.error) {
                        best = c;
                    }
                }
            }

            write_block(best, out.data() + (static_cast<std::size_t>(by) * cols + bx) *
                                              kEtc2BlockBytes);
        }
    }
    return true;
}

bool etc2_decode(const std::uint8_t* blocks, std::size_t block_bytes, int w, int h,
                 std::vector<std::uint8_t>& bgra_out) {
    if (blocks == nullptr || w <= 0 || h <= 0 || w % 4 != 0 || h % 4 != 0) {
        return false;
    }
    if (block_bytes < etc2_size(w, h)) {
        return false;
    }

    const int cols = w / 4;
    const int rows = h / 4;
    bgra_out.assign(static_cast<std::size_t>(w) * h * 4, 0);

    for (int by = 0; by < rows; ++by) {
        for (int bx = 0; bx < cols; ++bx) {
            const std::uint8_t* src =
                blocks + (static_cast<std::size_t>(by) * cols + bx) * kEtc2BlockBytes;
            std::uint64_t bits = 0;
            for (int i = 0; i < 8; ++i) {
                bits |= static_cast<std::uint64_t>(src[i]) << (56 - i * 8);
            }

            const bool differential = ((bits >> 33) & 1) != 0;
            const int flip = static_cast<int>((bits >> 32) & 1);

            std::array<Rgb, 2> base{};
            if (differential) {
                const int r = static_cast<int>((bits >> 59) & 0x1F);
                const int g = static_cast<int>((bits >> 51) & 0x1F);
                const int b = static_cast<int>((bits >> 43) & 0x1F);
                // Three-bit two's complement.
                const auto sign3 = [](int v) { return v >= 4 ? v - 8 : v; };
                const int dr = sign3(static_cast<int>((bits >> 56) & 0x07));
                const int dg = sign3(static_cast<int>((bits >> 48) & 0x07));
                const int db = sign3(static_cast<int>((bits >> 40) & 0x07));
                base[0] = Rgb{expand5(r), expand5(g), expand5(b)};
                base[1] = Rgb{expand5(std::clamp(r + dr, 0, 31)), expand5(std::clamp(g + dg, 0, 31)),
                              expand5(std::clamp(b + db, 0, 31))};
            } else {
                base[0] = Rgb{expand4(static_cast<int>((bits >> 60) & 0x0F)),
                              expand4(static_cast<int>((bits >> 52) & 0x0F)),
                              expand4(static_cast<int>((bits >> 44) & 0x0F))};
                base[1] = Rgb{expand4(static_cast<int>((bits >> 56) & 0x0F)),
                              expand4(static_cast<int>((bits >> 48) & 0x0F)),
                              expand4(static_cast<int>((bits >> 40) & 0x0F))};
            }

            const std::array<int, 2> table{static_cast<int>((bits >> 37) & 0x07),
                                           static_cast<int>((bits >> 34) & 0x07)};

            for (int x = 0; x < 4; ++x) {
                for (int y = 0; y < 4; ++y) {
                    const int pixel = x * 4 + y;
                    const int index = static_cast<int>(((bits >> (16 + pixel)) & 1) << 1 |
                                                       ((bits >> pixel) & 1));
                    const int half =
                        flip == 0 ? (x >= 2 ? 1 : 0) : (y >= 2 ? 1 : 0);
                    const int m = kModifiers[static_cast<std::size_t>(table[static_cast<std::size_t>(half)])]
                                            [static_cast<std::size_t>(index)];
                    const Rgb c = base[static_cast<std::size_t>(half)];

                    std::uint8_t* dst = bgra_out.data() +
                                        (static_cast<std::size_t>(by * 4 + y) * w +
                                         static_cast<std::size_t>(bx * 4 + x)) *
                                            4;
                    dst[0] = static_cast<std::uint8_t>(clamp255(c.b + m));
                    dst[1] = static_cast<std::uint8_t>(clamp255(c.g + m));
                    dst[2] = static_cast<std::uint8_t>(clamp255(c.r + m));
                    dst[3] = 255;
                }
            }
        }
    }
    return true;
}

} // namespace digitiz::host
