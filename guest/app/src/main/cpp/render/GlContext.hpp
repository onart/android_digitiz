#pragma once

// EGL surface bound to the GameActivity window.
//
// The window comes and goes with the activity lifecycle, so this is designed to
// be torn down and rebuilt at any time while the process keeps running.

#include <EGL/egl.h>

struct ANativeWindow;

namespace digitiz::guest {

class GlContext {
public:
    ~GlContext();

    bool attach(ANativeWindow* window);
    void detach();

    bool ready() const noexcept { return surface_ != EGL_NO_SURFACE; }

    // Re-reads the surface size. Cheap; call once per frame.
    void refresh_size();

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    void swap();

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLConfig config_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace digitiz::guest
