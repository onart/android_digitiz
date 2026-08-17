#include <memory>

#include <android/log.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <digitiz/core/log.hpp>

#include "App.hpp"

namespace {

constexpr const char* kLogTag = "digitiz";

int android_priority(digitiz::core::LogLevel level) {
    switch (level) {
    case digitiz::core::LogLevel::Trace:
        return ANDROID_LOG_VERBOSE;
    case digitiz::core::LogLevel::Debug:
        return ANDROID_LOG_DEBUG;
    case digitiz::core::LogLevel::Info:
        return ANDROID_LOG_INFO;
    case digitiz::core::LogLevel::Warn:
        return ANDROID_LOG_WARN;
    case digitiz::core::LogLevel::Error:
        return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_INFO;
}

digitiz::guest::App* app_from(android_app* app) {
    return static_cast<digitiz::guest::App*>(app->userData);
}

void handle_command(android_app* app, std::int32_t cmd) {
    if (auto* self = app_from(app)) {
        self->on_command(cmd);
    }
}

} // namespace

extern "C" void android_main(android_app* app) {
    digitiz::core::set_log_level(digitiz::core::LogLevel::Debug);
    digitiz::core::set_log_sink([](digitiz::core::LogLevel level, std::string_view text) {
        // %.*s: the sink hands out a view, which is not NUL-terminated.
        __android_log_print(android_priority(level), kLogTag, "%.*s",
                            static_cast<int>(text.size()), text.data());
    });

    DZ_INFO("guest starting");

    auto self = std::make_unique<digitiz::guest::App>(app);
    app->userData = self.get();
    app->onAppCmd = &handle_command;

    while (!app->destroyRequested) {
        android_poll_source* source = nullptr;
        int events = 0;

        // Sleep until something happens when there is nothing to draw; poll
        // without blocking while visible so the frame loop keeps running.
        //
        // The timeout is recomputed every iteration on purpose. Deciding once
        // and then looping on the poll would block forever the moment the app
        // lost focus: the command that restores focus arrives inside the loop,
        // but the stale timeout would keep it waiting for the next event.
        const int timeout = self->wants_frames() ? 0 : -1;

        if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            continue; // drain everything queued before drawing
        }

        self->frame();
    }

    DZ_INFO("guest stopping");
    app->userData = nullptr;
}
