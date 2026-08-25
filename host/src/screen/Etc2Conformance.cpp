// Checks the ETC2 bit layout against a decoder nobody here wrote.
//
// The encoder and the decoder in Etc2.cpp agree with each other, which proves
// they are consistent and not that either is correct: a layout misremembered
// the same way twice round-trips perfectly and renders as nonsense on a GPU.
// So the blocks go to a real ETC2 decoder — the desktop driver, which has to
// implement the same format the phone does, because both are implementing the
// same specification.
//
// Only ever run by hand, with --etc2-test.

#include "screen/Etc2Conformance.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <GLFW/glfw3.h>

#include <digitiz/core/log.hpp>

#include "screen/Etc2.hpp"

namespace digitiz::host {

namespace {

constexpr unsigned kCompressedRgb8Etc2 = 0x9274;

using PfnCompressedTexImage2D = void(APIENTRY*)(unsigned target, int level, unsigned format,
                                                int width, int height, int border, int size,
                                                const void* data);

// Something with flat fill, an edge, a gradient and colour, so a layout error
// in any of the fields has somewhere to show up.
std::vector<std::uint8_t> make_probe_image(int w, int h) {
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 255;
            int g = 255;
            int b = 255;
            if (x < w / 4) {
                r = g = b = 30; // flat dark
            } else if (x < w / 2) {
                r = 200; // flat colour
                g = 40;
                b = 90;
            } else if (x < 3 * w / 4) {
                r = g = b = (x % 3 == 0) ? 20 : 240; // hard vertical edges
            } else {
                r = x * 255 / std::max(w - 1, 1); // gradient
                g = y * 255 / std::max(h - 1, 1);
                b = 128;
            }
            std::uint8_t* p = bgra.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            p[0] = static_cast<std::uint8_t>(b);
            p[1] = static_cast<std::uint8_t>(g);
            p[2] = static_cast<std::uint8_t>(r);
            p[3] = 255;
        }
    }
    return bgra;
}

} // namespace

bool run_etc2_conformance() {
    constexpr int kW = 64;
    constexpr int kH = 64;

    if (!::glfwInit()) {
        DZ_ERROR("etc2 test: glfwInit failed");
        return false;
    }
    // ETC2 is core in desktop GL 4.3. Asking for it explicitly means a driver
    // that cannot decode this says so now rather than by returning zeroes.
    ::glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    ::glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    ::glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    ::glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = ::glfwCreateWindow(64, 64, "etc2", nullptr, nullptr);
    if (window == nullptr) {
        DZ_ERROR("etc2 test: no GL 4.3 context; cannot check against a driver decoder");
        ::glfwTerminate();
        return false;
    }
    ::glfwMakeContextCurrent(window);

    const auto compressed_tex_image_2d =
        reinterpret_cast<PfnCompressedTexImage2D>(::glfwGetProcAddress("glCompressedTexImage2D"));
    if (compressed_tex_image_2d == nullptr) {
        DZ_ERROR("etc2 test: glCompressedTexImage2D unavailable");
        ::glfwDestroyWindow(window);
        ::glfwTerminate();
        return false;
    }
    DZ_INFO("etc2 test: %s", reinterpret_cast<const char*>(::glGetString(GL_VERSION)));

    const std::vector<std::uint8_t> source = make_probe_image(kW, kH);

    std::vector<std::uint8_t> blocks;
    if (!etc2_encode(source.data(), kW, kH, kW * 4, blocks)) {
        DZ_ERROR("etc2 test: encode failed");
        return false;
    }

    std::vector<std::uint8_t> ours;
    if (!etc2_decode(blocks.data(), blocks.size(), kW, kH, ours)) {
        DZ_ERROR("etc2 test: decode failed");
        return false;
    }

    unsigned texture = 0;
    ::glGenTextures(1, &texture);
    ::glBindTexture(GL_TEXTURE_2D, texture);
    compressed_tex_image_2d(GL_TEXTURE_2D, 0, kCompressedRgb8Etc2, kW, kH, 0,
                            static_cast<int>(blocks.size()), blocks.data());
    const unsigned upload_error = ::glGetError();
    if (upload_error != 0) {
        DZ_ERROR("etc2 test: upload rejected (GL error 0x%04X)", upload_error);
        ::glDeleteTextures(1, &texture);
        ::glfwDestroyWindow(window);
        ::glfwTerminate();
        return false;
    }

    std::vector<std::uint8_t> driver(static_cast<std::size_t>(kW) * kH * 3, 0);
    ::glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, driver.data());
    const unsigned read_error = ::glGetError();
    ::glDeleteTextures(1, &texture);
    ::glfwDestroyWindow(window);
    ::glfwTerminate();

    if (read_error != 0) {
        DZ_ERROR("etc2 test: readback failed (GL error 0x%04X)", read_error);
        return false;
    }

    // Compare against our own decoder, not against the original: the question
    // is whether the bits mean the same thing to both, and any loss the
    // encoder chose to accept is shared by definition.
    int worst = 0;
    long long total = 0;
    for (int i = 0; i < kW * kH; ++i) {
        const std::uint8_t* mine = ours.data() + static_cast<std::size_t>(i) * 4;
        const std::uint8_t* theirs = driver.data() + static_cast<std::size_t>(i) * 3;
        const int d[3] = {std::abs(mine[2] - theirs[0]), std::abs(mine[1] - theirs[1]),
                          std::abs(mine[0] - theirs[2])};
        for (const int v : d) {
            worst = std::max(worst, v);
            total += v;
        }
    }

    const double mean = static_cast<double>(total) / (kW * kH * 3);
    if (worst == 0) {
        DZ_INFO("etc2 test: PASSED — the driver decodes every pixel exactly as we do");
        return true;
    }
    DZ_ERROR("etc2 test: FAILED — worst channel difference %d, mean %.3f. The bit layout does "
             "not mean to a real decoder what it means here.",
             worst, mean);
    return false;
}

} // namespace digitiz::host
