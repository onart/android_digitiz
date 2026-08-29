#include "ui/ImGuiShell.hpp"

#include <cstdio>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

namespace {

// Idle wake-up interval. Caps the refresh rate when nothing is happening.
constexpr double kIdleTimeoutSec = 1.0 / 30.0;

void on_glfw_error(int code, const char* description) {
    DZ_ERROR("glfw error %d: %s", code, description);
}

} // namespace

ImGuiShell::~ImGuiShell() {
    shutdown();
}

bool ImGuiShell::init(const char* title, int width, int height) {
    ::glfwSetErrorCallback(&on_glfw_error);

    if (!::glfwInit()) {
        DZ_ERROR("glfwInit failed");
        return false;
    }

    ::glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    ::glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    ::glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window_ = ::glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (window_ == nullptr) {
        DZ_ERROR("glfwCreateWindow failed");
        ::glfwTerminate();
        return false;
    }

    ::glfwMakeContextCurrent(window_);
    ::glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    load_font();

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
        DZ_ERROR("ImGui_ImplGlfw_InitForOpenGL failed");
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        DZ_ERROR("ImGui_ImplOpenGL3_Init failed");
        return false;
    }

    imgui_ready_ = true;
    return true;
}

// The window is in Korean, and ImGui's built-in font has no Hangul in it at
// all -- every label would come out as boxes. So a system font is loaded with
// the Korean glyph range.
//
// Bold rather than regular: this is a panel of small readouts against a dark
// background, and the regular weight of these faces goes thin and grey at the
// sizes involved. If neither face is there the built-in font stays, and the
// window is readable in the parts that are not Korean.
void ImGuiShell::load_font() {
    ImGuiIO& io = ImGui::GetIO();

    // Malgun Gothic Bold ships with Windows; Noto Sans KR is what a machine
    // that has had fonts installed on purpose is likely to have.
    static const char* kCandidates[] = {
        "C:/Windows/Fonts/malgunbd.ttf",
        "C:/Windows/Fonts/NotoSansKR-Bold.ttf",
        "C:/Windows/Fonts/NotoSansCJKkr-Bold.otf",
        "C:/Windows/Fonts/malgun.ttf",
    };

    for (const char* path : kCandidates) {
        std::FILE* f = std::fopen(path, "rb");
        if (f == nullptr) {
            continue;
        }
        std::fclose(f);
        // GetGlyphRangesKorean is Latin plus the 2350 syllables in common use,
        // not all 11172 -- the full set would be a much larger atlas for
        // syllables no interface writes.
        if (io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr,
                                         io.Fonts->GetGlyphRangesKorean()) != nullptr) {
            DZ_INFO("ui: font %s", path);
            return;
        }
    }
    DZ_WARN("ui: no Korean font found; labels will be boxes");
}

void ImGuiShell::shutdown() {
    if (imgui_ready_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    if (window_ != nullptr) {
        ::glfwDestroyWindow(window_);
        window_ = nullptr;
        ::glfwTerminate();
    }
}

bool ImGuiShell::begin_frame() {
    if (window_ == nullptr || ::glfwWindowShouldClose(window_)) {
        return false;
    }

    ::glfwWaitEventsTimeout(kIdleTimeoutSec);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    return true;
}

void ImGuiShell::end_frame() {
    ImGui::Render();

    int fb_w = 0;
    int fb_h = 0;
    ::glfwGetFramebufferSize(window_, &fb_w, &fb_h);
    ::glViewport(0, 0, fb_w, fb_h);
    ::glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
    ::glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ::glfwSwapBuffers(window_);
}

} // namespace digitiz::host
