#include "ui/ImGuiShell.hpp"

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
