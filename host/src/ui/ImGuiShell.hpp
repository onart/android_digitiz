#pragma once

// GLFW window + Dear ImGui lifecycle. Nothing app-specific lives here.

struct GLFWwindow;

namespace digitiz::host {

class ImGuiShell {
public:
    ~ImGuiShell();

    bool init(const char* title, int width, int height);
    void shutdown();

    // Returns false once the user has asked to close the window.
    // Blocks until an event arrives or the idle timeout expires, so an idle
    // host does not spin a render loop at display refresh rate.
    bool begin_frame();
    void end_frame();

    GLFWwindow* window() const noexcept { return window_; }

private:
    GLFWwindow* window_ = nullptr;
    bool imgui_ready_ = false;
};

} // namespace digitiz::host
