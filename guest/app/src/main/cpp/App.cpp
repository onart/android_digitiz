#include "App.hpp"

#include <GLES3/gl3.h>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

App::App(android_app* app) : app_(app) {}

void App::on_command(std::int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        gl_.attach(app_->window);
        break;

    case APP_CMD_TERM_WINDOW:
        gl_.detach();
        break;

    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        gl_.refresh_size();
        break;

    case APP_CMD_GAINED_FOCUS:
        has_focus_ = true;
        DZ_DEBUG("gained focus");
        break;

    case APP_CMD_LOST_FOCUS:
        has_focus_ = false;
        DZ_DEBUG("lost focus");
        break;

    default:
        break;
    }
}

void App::frame() {
    if (!gl_.ready()) {
        return;
    }
    render();
    gl_.swap();
}

void App::render() {
    glViewport(0, 0, gl_.width(), gl_.height());
    glClearColor(0.07f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

} // namespace digitiz::guest
