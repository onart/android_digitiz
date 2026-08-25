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
inline constexpr std::size_t kKeyNameBytes = 16;

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

// The coordinates are an offset inside the host's focused window rather than
// desktop pixels. Custom buttons use this: a button that means "the save icon"
// has to keep meaning that when the window is moved or opened on another
// monitor, and a desktop coordinate does not.
//
// Resolved against the visible frame of whatever has the focus at the moment
// the message arrives. If nothing does, the event is dropped rather than
// guessed at -- clicking the wrong place on someone's desktop is worse than
// not clicking.
inline constexpr std::uint8_t kPointerWindowRelative = 1u << 2;

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

// --- 0x20 VIEWPORT_REQ (C->H) ----------------------------------------------

enum class FrameFormat : std::uint8_t {
    // Nothing is sent. The guest is drawing the grid on its own.
    Off = 0,
    // 4x4 blocks, 4 bits per pixel, then zstd over the whole batch. Required by
    // GLES 3.0, so the guest can always decode it, and it decodes by being
    // handed to the GPU rather than by being decoded at all.
    Etc2Rgb8 = 1,
    // Same idea, twice the size, noticeably better on text. GLES 3.2.
    Astc4x4 = 2,
    // The step up when block compression cannot fit the link.
    H264 = 3,
};

const char* to_string(FrameFormat format) noexcept;

// What the guest wants to see. Sent on connect and whenever the view or the
// settings change; the host holds the last one and serves it until told
// otherwise.
struct ViewportReq {
    // The desktop region the guest is showing, in PC pixels. May be negative.
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    // The size to encode at, which is the resolution ratio the user chose.
    // Rounded up to a multiple of 4 by the host: block formats need it.
    std::uint16_t out_w = 0;
    std::uint16_t out_h = 0;
    // Upper bound on frames per second. Zero stops the stream without
    // forgetting the rest of the request.
    std::uint8_t fps = 0;
    FrameFormat format = FrameFormat::Off;
    // Tile edge in pixels, absolute rather than a fraction of the region: a
    // bigger region should mean more tiles, not bigger ones, or the cost of a
    // tile stops being predictable. Zero lets the host choose.
    std::uint8_t tile = 0;
    std::uint8_t flags = 0; // bit0: draw the cursor into the image
};

inline constexpr std::uint8_t kViewportCursor = 1u << 0;

// --- 0x21 FRAME_INFO (H->C) ------------------------------------------------

// Opens a batch of tiles. The bytes follow in FRAME_DATA chunks.
//
// Only tiles that changed are sent, and only as many per frame as the budget
// allows, so a batch is a patch and not a picture. Which tiles is the trailing
// index list, numbered left to right and top to bottom across the encoded
// surface.
struct FrameInfo {
    std::uint32_t seq = 0;
    // Echoed from the request being served, so a batch that crosses a viewport
    // change is recognisable as belonging to the old one.
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    std::uint16_t out_w = 0;
    std::uint16_t out_h = 0;
    FrameFormat format = FrameFormat::Off;
    std::uint8_t tile = 0;
    // Size the payload decompresses to, so the guest can size its buffer once.
    std::uint32_t raw_bytes = 0;
    std::uint32_t payload_bytes = 0;
    std::vector<std::uint16_t> tiles;
};

// --- 0x22 FRAME_DATA (H->C) ------------------------------------------------

// One slice of the payload named by the FRAME_INFO with the same seq.
//
// Split small on purpose. A priority queue above the socket cannot help once
// bytes are in the kernel buffer, so the chunk size is what actually bounds
// how long a pointer message can be stuck behind a frame: at USB speeds 16 KiB
// is about 0.65 ms.
inline constexpr std::size_t kFrameChunkBytes = 16u * 1024u;

struct FrameData {
    std::uint32_t seq = 0;
    std::uint32_t offset = 0;
    std::vector<std::byte> bytes;
};

// --- 0x23 ACTIVE_WINDOW (H->C) ---------------------------------------------

// Sent whenever the PC's focused window changes, so the guest can bring up the
// button preset that belongs to the program being used.
//
// The executable name only, not the window title: the title changes with the
// open document and would make a preset flap on every file the user opens,
// while the program is the thing a preset is actually about.
struct ActiveWindow {
    std::uint32_t pid = 0;
    // Bare file name, e.g. "krita.exe". Empty when the host could not identify
    // the window — a preset should fall back to its default rather than stay
    // on whatever was last matched.
    std::string process; // wire: kProcessNameBytes, NUL padded
};

// --- 0x24 KEY (C->H) -------------------------------------------------------

// Key::modifiers
inline constexpr std::uint8_t kModCtrl = 1u << 0;
inline constexpr std::uint8_t kModShift = 1u << 1;
inline constexpr std::uint8_t kModAlt = 1u << 2;
inline constexpr std::uint8_t kModMeta = 1u << 3; // Windows key / Command

enum class KeyAction : std::uint8_t {
    Press = 0, // down and up, with the modifiers held around it
    Down = 1,
    Up = 2,
};

// A shortcut fired by a custom button on the guest.
//
// The key travels as a name, not a code. Virtual-key numbering is a property
// of the host OS, and the guest has no business knowing it: the phone stores
// what the user typed and the host maps it to whatever its own input API
// wants. It also means a Linux or macOS host slots in without the guest
// changing at all.
struct Key {
    std::uint8_t modifiers = 0;
    KeyAction action = KeyAction::Press;
    // Lowercase, no modifiers in it: "a", "f5", "escape", "tab", "left".
    // wire: kKeyNameBytes, NUL padded.
    std::string key;
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
std::vector<std::byte> encode(const ActiveWindow& m);
std::vector<std::byte> encode(const Key& m);
std::vector<std::byte> encode(const ViewportReq& m);
std::vector<std::byte> encode(const FrameInfo& m);
std::vector<std::byte> encode(const FrameData& m);
std::vector<std::byte> encode(const LogMessage& m);

bool decode(std::span<const std::byte> payload, Hello& out);
bool decode(std::span<const std::byte> payload, HelloAck& out);
bool decode(std::span<const std::byte> payload, Ping& out);
bool decode(std::span<const std::byte> payload, Pong& out);
bool decode(std::span<const std::byte> payload, Pointer& out);
bool decode(std::span<const std::byte> payload, HostState& out);
bool decode(std::span<const std::byte> payload, ActiveWindow& out);
bool decode(std::span<const std::byte> payload, Key& out);
bool decode(std::span<const std::byte> payload, ViewportReq& out);
bool decode(std::span<const std::byte> payload, FrameInfo& out);
bool decode(std::span<const std::byte> payload, FrameData& out);
bool decode(std::span<const std::byte> payload, LogMessage& out);

} // namespace digitiz::proto
