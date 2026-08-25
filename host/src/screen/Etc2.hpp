#pragma once

// ETC2 RGB8: 4x4 blocks, 8 bytes each, four bits per pixel.
//
// Chosen because OpenGL ES 3.0 requires it, which is the version the guest
// already asks for, so every phone that can run the app can decode this — and
// decodes it by handing it to the GPU rather than by decoding it at all.
//
// The ratio is fixed at 8:1 from BGRA8 and does not depend on the content.
// That is the property the tile budget is built on: a batch of N tiles is
// exactly N * (tile/4)^2 * 8 bytes before zstd, known before anything is
// compressed.
//
// What is implemented is the ETC1 subset, which is a valid ETC2 stream and
// what every ETC2 decoder starts with. Its model is one base colour per
// half-block plus a per-pixel luminance offset, which suits screen content
// better than it sounds: flat fills are exact to within the smallest modifier,
// and antialiased black-on-white text is pure luminance variation. Where it
// gives up is two different hues inside one 4x4 block -- coloured text on a
// coloured background. ETC2's own T, H and planar modes are the answer to that
// and are the quality step up, before ASTC.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace digitiz::host {

inline constexpr int kEtc2BlockSize = 4;
inline constexpr std::size_t kEtc2BlockBytes = 8;

// Bytes a w*h image takes once encoded. Both must be multiples of 4.
constexpr std::size_t etc2_size(int w, int h) noexcept {
    if (w <= 0 || h <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(w / kEtc2BlockSize) *
           static_cast<std::size_t>(h / kEtc2BlockSize) * kEtc2BlockBytes;
}

// `bgra` is what the capture hands over: four bytes per pixel, blue first,
// alpha ignored. `stride` is bytes per row. Width and height must be multiples
// of four; the caller rounds the encoded size up, which is why the protocol
// says so too.
//
// Blocks come out in reading order, left to right and top to bottom.
bool etc2_encode(const std::uint8_t* bgra, int w, int h, int stride,
                 std::vector<std::uint8_t>& out);

// The other direction, for testing. The guest never calls this -- its GPU
// samples the compressed texture directly -- but an encoder whose output
// nobody has looked at is an encoder nobody knows is right.
bool etc2_decode(const std::uint8_t* blocks, std::size_t block_bytes, int w, int h,
                 std::vector<std::uint8_t>& bgra_out);

} // namespace digitiz::host
