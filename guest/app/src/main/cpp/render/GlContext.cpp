#include "render/GlContext.hpp"

#include <android/native_window.h>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

GlContext::~GlContext() {
    detach();
    if (display_ != EGL_NO_DISPLAY) {
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
}

bool GlContext::attach(ANativeWindow* window) {
    if (window == nullptr) {
        return false;
    }
    detach();

    if (display_ == EGL_NO_DISPLAY) {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY) {
            DZ_ERROR("eglGetDisplay failed");
            return false;
        }
        if (eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
            DZ_ERROR("eglInitialize failed: 0x%x", eglGetError());
            display_ = EGL_NO_DISPLAY;
            return false;
        }
    }

    if (config_ == nullptr) {
        const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_DEPTH_SIZE,      0,
            EGL_STENCIL_SIZE,    0,
            EGL_NONE,
        };
        EGLint count = 0;
        if (eglChooseConfig(display_, attribs, &config_, 1, &count) != EGL_TRUE || count < 1) {
            DZ_ERROR("eglChooseConfig found no ES3 config: 0x%x", eglGetError());
            config_ = nullptr;
            return false;
        }

        // The window buffer format must match the config we picked.
        EGLint native_visual = 0;
        eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &native_visual);
        ANativeWindow_setBuffersGeometry(window, 0, 0, native_visual);
    }

    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        DZ_ERROR("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return false;
    }

    if (context_ == EGL_NO_CONTEXT) {
        const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
        if (context_ == EGL_NO_CONTEXT) {
            DZ_ERROR("eglCreateContext failed: 0x%x", eglGetError());
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
            return false;
        }
    }

    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
        DZ_ERROR("eglMakeCurrent failed: 0x%x", eglGetError());
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
        return false;
    }

    // Tear before latency: this is a drawing surface, not a game.
    eglSwapInterval(display_, 1);

    refresh_size();
    DZ_INFO("EGL surface ready: %d x %d", width_, height_);
    return true;
}

void GlContext::detach() {
    if (display_ == EGL_NO_DISPLAY) {
        return;
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    // The context outlives the surface so GL objects survive a window bounce.
}

void GlContext::refresh_size() {
    if (!ready()) {
        return;
    }
    EGLint w = 0;
    EGLint h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    width_ = w;
    height_ = h;
}

void GlContext::swap() {
    if (ready()) {
        eglSwapBuffers(display_, surface_);
    }
}

} // namespace digitiz::guest
