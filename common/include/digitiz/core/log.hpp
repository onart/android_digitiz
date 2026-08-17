#pragma once

// Logging is a level + a sink. The host points the sink at its ImGui console;
// the guest points it at logcat *and* the LOG wire message, so both sides end
// up on one timeline in the host console.

#include <cstdint>
#include <functional>
#include <string_view>

namespace digitiz::core {

enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

const char* to_string(LogLevel level) noexcept;

using LogSink = std::function<void(LogLevel, std::string_view)>;

// Not thread-safe against concurrent log_write; set this once during startup.
void set_log_sink(LogSink sink);

// Messages below the minimum level are dropped before reaching the sink.
void set_log_level(LogLevel minimum);
LogLevel log_level() noexcept;

void log_write(LogLevel level, std::string_view text);

// snprintf semantics. Deliberately not std::format: libc++ support across the
// NDK versions we care about is not something to bet the build on.
#if defined(__GNUC__) || defined(__clang__)
[[gnu::format(printf, 2, 3)]]
#endif
void log_printf(LogLevel level, const char* fmt, ...);

} // namespace digitiz::core

#define DZ_TRACE(...) ::digitiz::core::log_printf(::digitiz::core::LogLevel::Trace, __VA_ARGS__)
#define DZ_DEBUG(...) ::digitiz::core::log_printf(::digitiz::core::LogLevel::Debug, __VA_ARGS__)
#define DZ_INFO(...) ::digitiz::core::log_printf(::digitiz::core::LogLevel::Info, __VA_ARGS__)
#define DZ_WARN(...) ::digitiz::core::log_printf(::digitiz::core::LogLevel::Warn, __VA_ARGS__)
#define DZ_ERROR(...) ::digitiz::core::log_printf(::digitiz::core::LogLevel::Error, __VA_ARGS__)
