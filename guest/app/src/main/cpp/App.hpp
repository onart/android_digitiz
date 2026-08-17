#pragma once

// Guest application. Owns the GL surface and the frame loop.

#include <cstdint>

#include <game-activity/native_app_glue/android_native_app_glue.h>

#include "render/GlContext.hpp"

namespace digitiz::guest {

class App {
public:
    explicit App(android_app* app);

    void on_command(std::int32_t cmd);
    void frame();

    bool has_focus() const noexcept { return has_focus_; }

private:
    void render();

    android_app* app_;
    GlContext gl_;
    bool has_focus_ = false;
};

} // namespace digitiz::guest
