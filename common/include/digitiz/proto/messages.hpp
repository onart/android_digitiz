#pragma once

// Message payloads. See docs/PROTOCOL.md for the wire layout of each.
//
// These are in-memory types, not wire images: strings are std::string here and
// fixed-width NUL-padded fields on the wire. encode()/decode() bridge the two.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <digitiz/core/log.hpp>
#include <digitiz/proto/wire.hpp>

namespace digitiz::proto {

inline constexpr std::size_t kDeviceNameBytes = 64;
inline constexpr std::size_t kProcessNameBytes = 64;

enum class HostOs : std::uint8_t { Windows = 0, Linux = 1, MacOS = 2 };

enum class PointerAction : std::uint8_t {
    Down = 0,
    Move = 1,
    Up = 2,
    Cancel = 3,
    Hover = 4, // reserved
};

enum class MouseButton : std::uint8_t { Left = 0, Right = 1, Middle = 2 };

// Pointer::flags
//
// Relative pointers carry a delta instead of a position: the guest does not
// know where the cursor is, only the host does. The start bit marks the first
// event of a gesture, telling the host to re-read the real cursor position
// before accumulating — the user may have moved a physical mouse in between.
inline constexpr std::uint8_t kPointerRelative = 1u << 0;
inline constexpr std::uint8_t kPointerGestureStart = 1u << 1;

const char* to_string(PointerAction action) noexcept;
const char* to_string(MouseButton button) noexcept;
const char* to_string(HostOs os) noexcept;

// --- 0x01 HELLO (C->H) -----------------------------------------------------

struct Hello {
    std::uint16_t proto_ver = kProtocolVersion;
    std::int32_t screen_w = 0;
    std::int32_t screen_h = 0;
    float density = 1.0f; // dpi / 160
    std::string device;   // wire: kDeviceNameBytes, NUL padded
};

// --- 0x02 HELLO_ACK (H->C) -------------------------------------------------

struct Monitor {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    std::uint32_t dpi = 96;
    bool primary = false;
};

struct HelloAck {
    std::uint16_t proto_ver = kProtocolVersion;
    HostOs host_os = HostOs::Windows;
    // Virtual desktop bounds. vx/vy go negative on multi-monitor layouts.
    std::int32_t vx = 0;
    std::int32_t vy = 0;
    std::int32_t vw = 0;
    std::int32_t vh = 0;
    std::vector<Monitor> monitors;
};

// --- 0x03 PING / 0x04 PONG -------------------------------------------------

struct Ping {
    std::uint64_t t_send_us = 0;
};

struct Pong {
    std::uint64_t t_send_us = 0; // echoed from the Ping
    std::uint64_t t_reply_us = 0;
};

// --- 0x10 POINTER (C->H) ---------------------------------------------------

struct Pointer {
    std::uint64_t t_us = 0; // guest monotonic clock
    // Absolute: PC virtual-desktop pixels, may be negative.
    // Relative (see kPointerRelative): a delta in PC pixels.
    std::int32_t x = 0;
    std::int32_t y = 0;
    PointerAction action = PointerAction::Move;
    MouseButton button = MouseButton::Left;
    std::uint8_t pointer_id = 0;
    std::uint8_t flags = 0;
    float pressure = 1.0f;
};

// --- 0x11 HOST_STATE (H->C) ------------------------------------------------

struct HostState {
    bool enabled = false;
    bool injecting = false;
};

// --- 0x7F LOG (both) -------------------------------------------------------

struct LogMessage {
    core::LogLevel level = core::LogLevel::Info;
    std::string text;
};

// ---------------------------------------------------------------------------
// Each encode() returns a complete framed message (header + payload).
// Each decode() takes the payload only, as handed out by Framer::drain().

std::vector<std::byte> encode(const Hello& m);
std::vector<std::byte> encode(const HelloAck& m);
std::vector<std::byte> encode(const Ping& m);
std::vector<std::byte> encode(const Pong& m);
std::vector<std::byte> encode(const Pointer& m);
std::vector<std::byte> encode(const HostState& m);
std::vector<std::byte> encode(const LogMessage& m);

bool decode(std::span<const std::byte> payload, Hello& out);
bool decode(std::span<const std::byte> payload, HelloAck& out);
bool decode(std::span<const std::byte> payload, Ping& out);
bool decode(std::span<const std::byte> payload, Pong& out);
bool decode(std::span<const std::byte> payload, Pointer& out);
bool decode(std::span<const std::byte> payload, HostState& out);
bool decode(std::span<const std::byte> payload, LogMessage& out);

} // namespace digitiz::proto
