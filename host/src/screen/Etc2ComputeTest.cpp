// Checks the compute encoder against the one it is replacing.
//
// Byte-for-byte agreement is deliberately not the bar. The two search the same
// candidates in the same order, but ties can fall either way and nothing
// downstream cares which -- the guest hands the bytes to its own GPU and never
// looks at them. What matters is that the blocks decode, and decode to a
// picture no worse than the CPU encoder's. So this decodes both with the same
// decoder and compares the error.
//
// It builds its own D3D11 device rather than borrowing the capture's, so it
// runs with no desktop duplication and no phone.

#include "screen/Etc2ComputeTest.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>

#include <digitiz/core/log.hpp>

#include "screen/Etc2.hpp"
#include "screen/Etc2Compute.hpp"

namespace digitiz::host {

namespace {

struct Image {
    std::string name;
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> bgra;
};

std::uint8_t clamp_byte(int v) {
    return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

Image make_image(const std::string& name, int w, int h, int kind) {
    Image im;
    im.name = name;
    im.w = w;
    im.h = h;
    im.bgra.resize(static_cast<std::size_t>(w) * h * 4);

    std::uint32_t seed = 12345;
    const auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<int>((seed >> 16) & 0xFF);
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 0;
            int g = 0;
            int b = 0;
            switch (kind) {
            case 0: // flat
                r = 32;
                g = 96;
                b = 200;
                break;
            case 1: // gradient
                r = x * 255 / w;
                g = y * 255 / h;
                b = 128;
                break;
            case 2: { // text-like: hard black on white, thin strokes
                const bool ink = ((x % 7) < 2 && (y % 11) < 8) || ((y % 11) == 8 && (x % 7) < 5);
                r = g = b = ink ? 20 : 240;
                break;
            }
            default: // noise, the floor
                r = rnd();
                g = rnd();
                b = rnd();
                break;
            }
            std::uint8_t* p = im.bgra.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            p[0] = clamp_byte(b);
            p[1] = clamp_byte(g);
            p[2] = clamp_byte(r);
            p[3] = 255;
        }
    }
    return im;
}

// Mean squared error per channel between the source and a decoded block run.
double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) {
        return -1.0;
    }
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const double d = static_cast<double>(a[i + c]) - static_cast<double>(b[i + c]);
            sum += d * d;
            ++n;
        }
    }
    if (n == 0 || sum <= 0.0) {
        return 99.0; // lossless
    }
    return 10.0 * std::log10(255.0 * 255.0 * n / sum);
}

template <typename T>
void release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

bool run_etc2_compute_test() {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 1,
                                   D3D11_SDK_VERSION, &device, &level, &context))) {
        DZ_ERROR("etc2 compute test: no Direct3D 11 device");
        return false;
    }

    std::unique_ptr<Etc2ComputeEncoder> encoder = Etc2ComputeEncoder::create(device);
    if (!encoder) {
        release(context);
        release(device);
        return false;
    }

    bool ok = true;
    const int kSize = 128;

    for (int kind = 0; kind < 4; ++kind) {
        static const char* names[] = {"flat", "gradient", "text", "noise"};
        const Image im = make_image(names[kind], kSize, kSize, kind);

        // Upload as the capture's own format, so the shader reads exactly what
        // it will read in earnest.
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(im.w);
        desc.Height = static_cast<UINT>(im.h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = im.bgra.data();
        init.SysMemPitch = static_cast<UINT>(im.w) * 4;

        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(device->CreateTexture2D(&desc, &init, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture, nullptr, &srv))) {
            DZ_ERROR("etc2 compute test: could not upload %s", im.name.c_str());
            release(srv);
            release(texture);
            ok = false;
            break;
        }

        // Two jobs: one at native size, one halved, so the box average is
        // exercised as well as the encoder.
        const TileJob jobs[2] = {
            TileJob{core::Recti{0, 0, kSize, kSize}, kSize, kSize},
            TileJob{core::Recti{0, 0, kSize, kSize}, kSize / 2, kSize / 2},
        };

        std::vector<std::uint8_t> gpu;
        if (!encoder->encode(srv, jobs, gpu)) {
            DZ_ERROR("etc2 compute test: encode failed on %s", im.name.c_str());
            release(srv);
            release(texture);
            ok = false;
            break;
        }
        release(srv);
        release(texture);

        const std::size_t full = etc2_size(kSize, kSize);
        if (gpu.size() != full + etc2_size(kSize / 2, kSize / 2)) {
            DZ_ERROR("etc2 compute test: %s produced %zu bytes, expected %zu", im.name.c_str(),
                     gpu.size(), full + etc2_size(kSize / 2, kSize / 2));
            ok = false;
            break;
        }

        std::vector<std::uint8_t> cpu;
        if (!etc2_encode(im.bgra.data(), im.w, im.h, im.w * 4, cpu)) {
            DZ_ERROR("etc2 compute test: the CPU encoder refused %s", im.name.c_str());
            ok = false;
            break;
        }

        std::vector<std::uint8_t> gpu_px;
        std::vector<std::uint8_t> cpu_px;
        if (!etc2_decode(gpu.data(), full, kSize, kSize, gpu_px) ||
            !etc2_decode(cpu.data(), cpu.size(), kSize, kSize, cpu_px)) {
            DZ_ERROR("etc2 compute test: %s would not decode", im.name.c_str());
            ok = false;
            break;
        }

        const double gpu_db = psnr(im.bgra, gpu_px);
        const double cpu_db = psnr(im.bgra, cpu_px);
        const std::size_t same = [&] {
            std::size_t n = 0;
            for (std::size_t i = 0; i < full; ++i) {
                n += gpu[i] == cpu[i] ? 1 : 0;
            }
            return n;
        }();

        DZ_INFO("etc2 compute: %-8s gpu %.1f dB, cpu %.1f dB, %zu%% of bytes identical",
                im.name.c_str(), gpu_db, cpu_db, same * 100 / full);

        // Half a decibel of slack for ties broken the other way. Anything
        // larger is a real disagreement about the encoding, not a coin flip.
        if (gpu_db < cpu_db - 0.5) {
            DZ_ERROR("etc2 compute: %s is worse on the GPU by %.2f dB", im.name.c_str(),
                     cpu_db - gpu_db);
            ok = false;
        }
    }

    DZ_INFO("etc2 compute: last dispatch %.0f us, map %.0f us", encoder->dispatch_us(),
            encoder->map_us());

    encoder.reset();
    release(context);
    release(device);
    DZ_INFO("etc2 compute test %s", ok ? "PASSED" : "FAILED");
    return ok;
}

} // namespace digitiz::host
