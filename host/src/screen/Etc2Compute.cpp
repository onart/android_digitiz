#include "screen/Etc2Compute.hpp"

#include <chrono>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3dcompiler.h>

#include <digitiz/core/log.hpp>

#include "screen/Etc2.hpp"

namespace digitiz::host {

namespace {

// The same encoder as Etc2.cpp, block for block and bit for bit.
//
// Two things are worth knowing before reading it. ETC numbers the pixels of a
// block down the columns, not across the rows, so the index is x * 4 + y. And
// the 64 bits are written most significant byte first, which is why the two
// halves are byte-swapped on the way out -- HLSL has no 64-bit integer here,
// so they are carried as a high and a low word throughout.
constexpr const char* kShaderSrc = R"(
Texture2D<float4> gSrc : register(t0);
// Two int4 per job: [0] is src x, y, w, h and [1] is out w, out h, the byte
// offset of the tile's first block, unused.
StructuredBuffer<int4> gJobs : register(t1);

RWByteAddressBuffer gOut : register(u0);

static const int kMod[8][4] = {
    {  2,   8,  -2,   -8},
    {  5,  17,  -5,  -17},
    {  9,  29,  -9,  -29},
    { 13,  42, -13,  -42},
    { 18,  60, -18,  -60},
    { 24,  80, -24,  -80},
    { 33, 106, -33, -106},
    { 47, 183, -47, -183},
};

int clamp255(int v) { return clamp(v, 0, 255); }
int quant4(int v)   { return clamp((v * 15 + 127) / 255, 0, 15); }
int expand4(int q)  { return q * 17; }
int quant5(int v)   { return clamp((v * 31 + 127) / 255, 0, 31); }
int expand5(int q)  { return (q << 3) | (q >> 2); }

uint bswap(uint v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

// Which of the sixteen pixels belong to `half`, in the order the CPU encoder
// visits them: x outer, y inner.
void subblock(int flip, int half, out int pix[8]) {
    int n = 0;
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            bool mine = (flip == 0) ? ((x >= 2) == (half == 1)) : ((y >= 2) == (half == 1));
            if (mine) {
                pix[n++] = x * 4 + y;
            }
        }
    }
}

// Best modifier table and per-pixel indices for one half against one base
// colour, and what that costs.
int fit(int3 px[8], int3 base, out int table_out, out int idx_out[8]) {
    int best_error = -1;
    table_out = 0;
    for (int i = 0; i < 8; ++i) {
        idx_out[i] = 0;
    }
    for (int table = 0; table < 8; ++table) {
        int error = 0;
        int indices[8];
        for (int i = 0; i < 8; ++i) {
            int best_pixel = -1;
            int best_index = 0;
            for (int index = 0; index < 4; ++index) {
                int m = kMod[table][index];
                int dr = clamp255(base.r + m) - px[i].r;
                int dg = clamp255(base.g + m) - px[i].g;
                int db = clamp255(base.b + m) - px[i].b;
                int e = dr * dr + dg * dg + db * db;
                if (best_pixel < 0 || e < best_pixel) {
                    best_pixel = e;
                    best_index = index;
                }
            }
            indices[i] = best_index;
            error += best_pixel;
        }
        if (best_error < 0 || error < best_error) {
            best_error = error;
            table_out = table;
            for (int j = 0; j < 8; ++j) {
                idx_out[j] = indices[j];
            }
        }
    }
    return best_error;
}

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    int4 a = gJobs[group.x * 2 + 0];
    int4 b = gJobs[group.x * 2 + 1];
    int2 src_xy = a.xy;
    int2 src_wh = a.zw;
    int2 out_wh = b.xy;
    uint base_byte = (uint)b.z;

    int cols = out_wh.x / 4;
    int rows = out_wh.y / 4;
    int total = cols * rows;

    for (int blk = (int)thread.x; blk < total; blk += 64) {
        int bx = blk % cols;
        int by = blk / cols;

        // Box average down to the block's sixteen pixels, the same way the CPU
        // path does it: nearest sampling drops whole strokes out of text.
        int3 px[16];
        for (int py = 0; py < 4; ++py) {
            for (int pxi = 0; pxi < 4; ++pxi) {
                int ox = bx * 4 + pxi;
                int oy = by * 4 + py;
                int sx0 = ox * src_wh.x / out_wh.x;
                int sx1 = max(sx0 + 1, (ox + 1) * src_wh.x / out_wh.x);
                int sy0 = oy * src_wh.y / out_wh.y;
                int sy1 = max(sy0 + 1, (oy + 1) * src_wh.y / out_wh.y);

                int3 sum = int3(0, 0, 0);
                int n = 0;
                for (int sy = sy0; sy < sy1; ++sy) {
                    for (int sx = sx0; sx < sx1; ++sx) {
                        float4 c = gSrc.Load(int3(src_xy.x + sx, src_xy.y + sy, 0));
                        sum += int3(int(c.r * 255.0f + 0.5f), int(c.g * 255.0f + 0.5f),
                                    int(c.b * 255.0f + 0.5f));
                        ++n;
                    }
                }
                n = max(n, 1);
                px[pxi * 4 + py] = sum / n;
            }
        }

        int best_error = -1;
        int best_flip = 0;
        int best_diff = 0;
        int3 best_q0 = int3(0, 0, 0);
        int3 best_q1 = int3(0, 0, 0);
        int best_table[2] = {0, 0};
        int best_idx0[8];
        int best_idx1[8];
        for (int z = 0; z < 8; ++z) {
            best_idx0[z] = 0;
            best_idx1[z] = 0;
        }

        for (int flip = 0; flip < 2; ++flip) {
            int3 half0[8];
            int3 half1[8];
            int pix[8];
            subblock(flip, 0, pix);
            for (int h0 = 0; h0 < 8; ++h0) {
                half0[h0] = px[pix[h0]];
            }
            subblock(flip, 1, pix);
            for (int h1 = 0; h1 < 8; ++h1) {
                half1[h1] = px[pix[h1]];
            }

            int3 avg0 = int3(0, 0, 0);
            int3 avg1 = int3(0, 0, 0);
            for (int s = 0; s < 8; ++s) {
                avg0 += half0[s];
                avg1 += half1[s];
            }
            avg0 = (avg0 + 4) / 8;
            avg1 = (avg1 + 4) / 8;

            // Individual: four bits a channel, the halves unrelated.
            {
                int3 q0 = int3(quant4(avg0.r), quant4(avg0.g), quant4(avg0.b));
                int3 q1 = int3(quant4(avg1.r), quant4(avg1.g), quant4(avg1.b));
                int3 base0 = int3(expand4(q0.r), expand4(q0.g), expand4(q0.b));
                int3 base1 = int3(expand4(q1.r), expand4(q1.g), expand4(q1.b));
                int t0, t1;
                int i0[8], i1[8];
                int error = fit(half0, base0, t0, i0) + fit(half1, base1, t1, i1);
                if (best_error < 0 || error < best_error) {
                    best_error = error;
                    best_flip = flip;
                    best_diff = 0;
                    best_q0 = q0;
                    best_q1 = q1;
                    best_table[0] = t0;
                    best_table[1] = t1;
                    for (int c0 = 0; c0 < 8; ++c0) {
                        best_idx0[c0] = i0[c0];
                        best_idx1[c0] = i1[c0];
                    }
                }
            }

            // Differential: five bits for the first half and a three-bit signed
            // step for the second. Finer when the halves are close, impossible
            // when they are not -- the step is clamped rather than abandoned,
            // and if the clamp hurts the individual candidate wins on error.
            {
                int3 bq = int3(quant5(avg0.r), quant5(avg0.g), quant5(avg0.b));
                int3 want = int3(quant5(avg1.r), quant5(avg1.g), quant5(avg1.b));
                int3 d = clamp(want - bq, -4, 3);
                int3 second = clamp(bq + d, 0, 31);
                d = second - bq;

                int3 base0 = int3(expand5(bq.r), expand5(bq.g), expand5(bq.b));
                int3 base1 = int3(expand5(second.r), expand5(second.g), expand5(second.b));
                int t0, t1;
                int i0[8], i1[8];
                int error = fit(half0, base0, t0, i0) + fit(half1, base1, t1, i1);
                if (best_error < 0 || error < best_error) {
                    best_error = error;
                    best_flip = flip;
                    best_diff = 1;
                    best_q0 = bq;
                    best_q1 = d;
                    best_table[0] = t0;
                    best_table[1] = t1;
                    for (int c1 = 0; c1 < 8; ++c1) {
                        best_idx0[c1] = i0[c1];
                        best_idx1[c1] = i1[c1];
                    }
                }
            }
        }

        // Bits 63..32 carry the colours, tables and flip; 31..0 the indices.
        uint hi = 0;
        uint lo = 0;
        if (best_diff != 0) {
            hi |= (uint(best_q0.r) & 0x1Fu) << 27;
            hi |= (uint(best_q1.r) & 0x07u) << 24;
            hi |= (uint(best_q0.g) & 0x1Fu) << 19;
            hi |= (uint(best_q1.g) & 0x07u) << 16;
            hi |= (uint(best_q0.b) & 0x1Fu) << 11;
            hi |= (uint(best_q1.b) & 0x07u) << 8;
            hi |= 1u << 1;
        } else {
            hi |= (uint(best_q0.r) & 0x0Fu) << 28;
            hi |= (uint(best_q1.r) & 0x0Fu) << 24;
            hi |= (uint(best_q0.g) & 0x0Fu) << 20;
            hi |= (uint(best_q1.g) & 0x0Fu) << 16;
            hi |= (uint(best_q0.b) & 0x0Fu) << 12;
            hi |= (uint(best_q1.b) & 0x0Fu) << 8;
        }
        hi |= (uint(best_table[0]) & 0x07u) << 5;
        hi |= (uint(best_table[1]) & 0x07u) << 2;
        hi |= uint(best_flip & 1);

        int pix0[8];
        int pix1[8];
        subblock(best_flip, 0, pix0);
        subblock(best_flip, 1, pix1);
        for (int w = 0; w < 8; ++w) {
            lo |= (uint(best_idx0[w] >> 1) & 1u) << (16 + pix0[w]);
            lo |= (uint(best_idx0[w]) & 1u) << pix0[w];
            lo |= (uint(best_idx1[w] >> 1) & 1u) << (16 + pix1[w]);
            lo |= (uint(best_idx1[w]) & 1u) << pix1[w];
        }

        gOut.Store2(base_byte + uint(blk) * 8u, uint2(bswap(hi), bswap(lo)));
    }
}
)";

template <typename T>
void release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

double us_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

Etc2ComputeEncoder::~Etc2ComputeEncoder() {
    release(jobs_srv_);
    release(jobs_);
    release(out_uav_);
    release(out_);
    release(readback_);
    release(shader_);
    release(context_);
}

std::unique_ptr<Etc2ComputeEncoder> Etc2ComputeEncoder::create(ID3D11Device* device) {
    if (device == nullptr) {
        return nullptr;
    }
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        // Raw buffer stores and cs_5_0 both want 11_0. Below that the CPU path
        // is the answer, which is why this returns null rather than failing.
        DZ_WARN("etc2 compute: feature level below 11_0");
        return nullptr;
    }

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr =
        ::D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "etc2.hlsl", nullptr, nullptr, "main",
                     "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        DZ_ERROR("etc2 compute: shader would not compile: %s",
                 errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer())
                                   : "no message");
        release(errors);
        release(code);
        return nullptr;
    }
    release(errors);

    std::unique_ptr<Etc2ComputeEncoder> self(new Etc2ComputeEncoder());
    self->device_ = device;
    device->GetImmediateContext(&self->context_);

    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                           &self->shader_))) {
        DZ_ERROR("etc2 compute: could not create the shader");
        release(code);
        return nullptr;
    }
    release(code);

    DZ_INFO("etc2 compute: encoder ready");
    return self;
}

bool Etc2ComputeEncoder::ensure_buffers(std::size_t job_count, std::size_t block_bytes) {
    if (job_count > jobs_capacity_) {
        release(jobs_srv_);
        release(jobs_);
        jobs_capacity_ = job_count + 32;

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(jobs_capacity_ * sizeof(int) * 8); // two int4 per job
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(int) * 4;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &jobs_))) {
            jobs_capacity_ = 0;
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = static_cast<UINT>(jobs_capacity_ * 2);
        if (FAILED(device_->CreateShaderResourceView(jobs_, &srv, &jobs_srv_))) {
            return false;
        }
    }

    if (block_bytes > out_capacity_) {
        release(out_uav_);
        release(out_);
        release(readback_);
        out_capacity_ = ((block_bytes * 2) + 4095) & ~std::size_t{4095};

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(out_capacity_);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &out_))) {
            out_capacity_ = 0;
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = static_cast<UINT>(out_capacity_ / 4);
        uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device_->CreateUnorderedAccessView(out_, &uav, &out_uav_))) {
            return false;
        }

        D3D11_BUFFER_DESC back{};
        back.ByteWidth = static_cast<UINT>(out_capacity_);
        back.Usage = D3D11_USAGE_STAGING;
        back.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateBuffer(&back, nullptr, &readback_))) {
            return false;
        }
    }
    return true;
}

bool Etc2ComputeEncoder::encode(ID3D11ShaderResourceView* source, std::span<const TileJob> jobs,
                                std::vector<std::uint8_t>& blocks) {
    if (source == nullptr || jobs.empty() || shader_ == nullptr) {
        return false;
    }

    // Byte offsets, computed the same way the CPU path concatenates them.
    std::vector<std::int32_t> packed(jobs.size() * 8, 0);
    std::size_t total = 0;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const TileJob& j = jobs[i];
        if (j.out_w <= 0 || j.out_h <= 0 || j.out_w % 4 != 0 || j.out_h % 4 != 0 || j.src.w <= 0 ||
            j.src.h <= 0) {
            return false;
        }
        packed[i * 8 + 0] = j.src.x;
        packed[i * 8 + 1] = j.src.y;
        packed[i * 8 + 2] = j.src.w;
        packed[i * 8 + 3] = j.src.h;
        packed[i * 8 + 4] = j.out_w;
        packed[i * 8 + 5] = j.out_h;
        packed[i * 8 + 6] = static_cast<std::int32_t>(total);
        total += etc2_size(j.out_w, j.out_h);
    }

    if (!ensure_buffers(jobs.size(), total)) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(jobs_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, packed.data(), packed.size() * sizeof(std::int32_t));
    context_->Unmap(jobs_, 0);

    const auto dispatch_started = std::chrono::steady_clock::now();

    ID3D11ShaderResourceView* srvs[2] = {source, jobs_srv_};
    context_->CSSetShader(shader_, nullptr, 0);
    context_->CSSetShaderResources(0, 2, srvs);
    UINT counts = 0;
    context_->CSSetUnorderedAccessViews(0, 1, &out_uav_, &counts);
    context_->Dispatch(static_cast<UINT>(jobs.size()), 1, 1);

    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ID3D11UnorderedAccessView* no_uav = nullptr;
    context_->CSSetShaderResources(0, 2, none);
    context_->CSSetUnorderedAccessViews(0, 1, &no_uav, &counts);

    D3D11_BOX box{};
    box.right = static_cast<UINT>(total);
    box.bottom = 1;
    box.back = 1;
    context_->CopySubresourceRegion(readback_, 0, 0, 0, 0, out_, 0, &box);
    dispatch_us_ = us_since(dispatch_started);

    const auto map_started = std::chrono::steady_clock::now();
    D3D11_MAPPED_SUBRESOURCE got{};
    if (FAILED(context_->Map(readback_, 0, D3D11_MAP_READ, 0, &got))) {
        return false;
    }
    blocks.resize(total);
    std::memcpy(blocks.data(), got.pData, total);
    context_->Unmap(readback_, 0);
    map_us_ = us_since(map_started);
    return true;
}

} // namespace digitiz::host
