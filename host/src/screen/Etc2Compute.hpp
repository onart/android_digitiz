#pragma once

// ETC2 encoding on the GPU, so that what crosses the bus is blocks and not
// pixels.
//
// The readback is what this whole path costs: pulling a region of BGRA across
// to encode it on the CPU is charged per batch and is the largest single
// number in the frame. Encoding where the pixels already live turns 4 bytes a
// pixel into half a byte, and takes the per-tile CPU cost away entirely.
//
// The shader implements the same ETC1 subset as the CPU encoder in Etc2.cpp,
// down to the bit layout -- it has to, because the guest hands the bytes
// straight to the GPU and never decodes them. It is deliberately not required
// to produce byte-identical output: what matters is that the blocks decode to
// a picture at least as good, and that is what the test checks.
//
// Windows only, like the capture it reads from. The CPU path stays as the
// fallback and as the reference.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>

#include <digitiz/core/geometry.hpp>

namespace digitiz::host {

// One tile to encode: where to sample it from, and how large it comes out.
struct TileJob {
    // Source rectangle in the texture's own coordinates.
    core::Recti src{};
    // Encoded size, both multiples of four. Smaller than `src` means a box
    // average; equal means a straight copy.
    int out_w = 0;
    int out_h = 0;
};

class Etc2ComputeEncoder {
public:
    ~Etc2ComputeEncoder();

    // Returns null if the shader will not compile or the device cannot run it,
    // which is a reason to use the CPU path rather than a reason to fail.
    static std::unique_ptr<Etc2ComputeEncoder> create(ID3D11Device* device);

    // Encodes every job out of `source` and concatenates the blocks in job
    // order, exactly as the CPU path lays them out.
    bool encode(ID3D11ShaderResourceView* source, std::span<const TileJob> jobs,
                std::vector<std::uint8_t>& blocks);

    // Split of the last encode, in microseconds. `dispatch_us` is the call
    // itself, which returns without waiting; `map_us` is where the CPU waits
    // for the GPU to have finished.
    double dispatch_us() const noexcept { return dispatch_us_; }
    double map_us() const noexcept { return map_us_; }

private:
    Etc2ComputeEncoder() = default;
    bool ensure_buffers(std::size_t job_count, std::size_t block_bytes);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11ComputeShader* shader_ = nullptr;

    ID3D11Buffer* jobs_ = nullptr;
    ID3D11ShaderResourceView* jobs_srv_ = nullptr;
    std::size_t jobs_capacity_ = 0;

    ID3D11Buffer* out_ = nullptr;
    ID3D11UnorderedAccessView* out_uav_ = nullptr;
    ID3D11Buffer* readback_ = nullptr;
    std::size_t out_capacity_ = 0;

    double dispatch_us_ = 0.0;
    double map_us_ = 0.0;
};

} // namespace digitiz::host
