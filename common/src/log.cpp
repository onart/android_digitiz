#include <digitiz/core/log.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace digitiz::core {

namespace {

LogSink g_sink;
LogLevel g_min_level = LogLevel::Debug;

} // namespace

const char* to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "?";
}

void set_log_sink(LogSink sink) {
    g_sink = std::move(sink);
}

void set_log_level(LogLevel minimum) {
    g_min_level = minimum;
}

LogLevel log_level() noexcept {
    return g_min_level;
}

void log_write(LogLevel level, std::string_view text) {
    if (level < g_min_level || !g_sink) {
        return;
    }
    g_sink(level, text);
}

void log_printf(LogLevel level, const char* fmt, ...) {
    if (level < g_min_level || !g_sink) {
        return;
    }

    char stack_buf[512];

    std::va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
    va_end(args);

    if (n < 0) {
        return;
    }
    if (static_cast<std::size_t>(n) < sizeof(stack_buf)) {
        g_sink(level, std::string_view(stack_buf, static_cast<std::size_t>(n)));
        return;
    }

    // Rare: retry once with an exact-sized buffer.
    std::vector<char> heap_buf(static_cast<std::size_t>(n) + 1);
    va_start(args, fmt);
    const int n2 = std::vsnprintf(heap_buf.data(), heap_buf.size(), fmt, args);
    va_end(args);
    if (n2 > 0) {
        g_sink(level, std::string_view(heap_buf.data(), static_cast<std::size_t>(n2)));
    }
}

} // namespace digitiz::core
