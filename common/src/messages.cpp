#include <digitiz/proto/messages.hpp>

namespace digitiz::proto {

const char* to_string(MsgType type) noexcept {
    switch (type) {
    case MsgType::Hello:
        return "HELLO";
    case MsgType::HelloAck:
        return "HELLO_ACK";
    case MsgType::Ping:
        return "PING";
    case MsgType::Pong:
        return "PONG";
    case MsgType::Pointer:
        return "POINTER";
    case MsgType::HostState:
        return "HOST_STATE";
    case MsgType::ViewportReq:
        return "VIEWPORT_REQ";
    case MsgType::FrameInfo:
        return "FRAME_INFO";
    case MsgType::FrameData:
        return "FRAME_DATA";
    case MsgType::ActiveWindow:
        return "ACTIVE_WINDOW";
    case MsgType::Key:
        return "KEY";
    case MsgType::Wheel:
        return "WHEEL";
    case MsgType::Smoothing:
        return "SMOOTHING";
    case MsgType::Log:
        return "LOG";
    }
    return "UNKNOWN";
}

const char* to_string(FrameFormat format) noexcept {
    switch (format) {
    case FrameFormat::Off:
        return "OFF";
    case FrameFormat::Etc2Rgb8:
        return "ETC2_RGB8";
    case FrameFormat::Astc4x4:
        return "ASTC_4x4";
    case FrameFormat::H264:
        return "H264";
    }
    return "?";
}

const char* to_string(PointerAction action) noexcept {
    switch (action) {
    case PointerAction::Down:
        return "DOWN";
    case PointerAction::Move:
        return "MOVE";
    case PointerAction::Up:
        return "UP";
    case PointerAction::Cancel:
        return "CANCEL";
    case PointerAction::Hover:
        return "HOVER";
    }
    return "?";
}

const char* to_string(MouseButton button) noexcept {
    switch (button) {
    case MouseButton::Left:
        return "LEFT";
    case MouseButton::Right:
        return "RIGHT";
    case MouseButton::Middle:
        return "MIDDLE";
    }
    return "?";
}

const char* to_string(HostOs os) noexcept {
    switch (os) {
    case HostOs::Windows:
        return "Windows";
    case HostOs::Linux:
        return "Linux";
    case HostOs::MacOS:
        return "macOS";
    }
    return "?";
}

// --- Hello -----------------------------------------------------------------

std::vector<std::byte> encode(const Hello& m) {
    MessageBuilder b(MsgType::Hello);
    b.w().u16(m.proto_ver);
    b.w().u16(0); // reserved
    b.w().i32(m.screen_w);
    b.w().i32(m.screen_h);
    b.w().f32(m.density);
    b.w().fixed_str(m.device, kDeviceNameBytes);
    return b.take();
}

bool decode(std::span<const std::byte> payload, Hello& out) {
    Reader r(payload);
    out.proto_ver = r.u16();
    r.u16(); // reserved
    out.screen_w = r.i32();
    out.screen_h = r.i32();
    out.density = r.f32();
    out.device = r.fixed_str(kDeviceNameBytes);
    return r.done();
}

// --- HelloAck --------------------------------------------------------------

std::vector<std::byte> encode(const HelloAck& m) {
    MessageBuilder b(MsgType::HelloAck);
    b.w().u16(m.proto_ver);
    b.w().u8(static_cast<std::uint8_t>(m.host_os));
    b.w().u8(static_cast<std::uint8_t>(m.monitors.size()));
    b.w().i32(m.vx);
    b.w().i32(m.vy);
    b.w().i32(m.vw);
    b.w().i32(m.vh);
    b.w().u32(0); // reserved
    for (const Monitor& mon : m.monitors) {
        b.w().i32(mon.x);
        b.w().i32(mon.y);
        b.w().i32(mon.w);
        b.w().i32(mon.h);
        b.w().u32(mon.dpi);
        b.w().boolean(mon.primary);
        b.w().pad(3);
    }
    return b.take();
}

bool decode(std::span<const std::byte> payload, HelloAck& out) {
    Reader r(payload);
    out.proto_ver = r.u16();
    out.host_os = static_cast<HostOs>(r.u8());
    const std::uint8_t count = r.u8();
    out.vx = r.i32();
    out.vy = r.i32();
    out.vw = r.i32();
    out.vh = r.i32();
    r.u32(); // reserved

    out.monitors.clear();
    out.monitors.reserve(count);
    for (std::uint8_t i = 0; i < count && r.ok(); ++i) {
        Monitor mon;
        mon.x = r.i32();
        mon.y = r.i32();
        mon.w = r.i32();
        mon.h = r.i32();
        mon.dpi = r.u32();
        mon.primary = r.boolean();
        r.skip(3);
        out.monitors.push_back(mon);
    }
    return r.done();
}

// --- Ping / Pong -----------------------------------------------------------

std::vector<std::byte> encode(const Ping& m) {
    MessageBuilder b(MsgType::Ping);
    b.w().u64(m.t_send_us);
    return b.take();
}

bool decode(std::span<const std::byte> payload, Ping& out) {
    Reader r(payload);
    out.t_send_us = r.u64();
    return r.done();
}

std::vector<std::byte> encode(const Pong& m) {
    MessageBuilder b(MsgType::Pong);
    b.w().u64(m.t_send_us);
    b.w().u64(m.t_reply_us);
    return b.take();
}

bool decode(std::span<const std::byte> payload, Pong& out) {
    Reader r(payload);
    out.t_send_us = r.u64();
    out.t_reply_us = r.u64();
    return r.done();
}

// --- Pointer ---------------------------------------------------------------

std::vector<std::byte> encode(const Pointer& m) {
    MessageBuilder b(MsgType::Pointer);
    b.w().u64(m.t_us);
    b.w().i32(m.x);
    b.w().i32(m.y);
    b.w().u8(static_cast<std::uint8_t>(m.action));
    b.w().u8(static_cast<std::uint8_t>(m.button));
    b.w().u8(m.pointer_id);
    b.w().u8(m.flags);
    b.w().f32(m.pressure);
    return b.take();
}

bool decode(std::span<const std::byte> payload, Pointer& out) {
    Reader r(payload);
    out.t_us = r.u64();
    out.x = r.i32();
    out.y = r.i32();
    out.action = static_cast<PointerAction>(r.u8());
    out.button = static_cast<MouseButton>(r.u8());
    out.pointer_id = r.u8();
    out.flags = r.u8();
    out.pressure = r.f32();
    return r.done();
}

// --- HostState -------------------------------------------------------------

std::vector<std::byte> encode(const HostState& m) {
    MessageBuilder b(MsgType::HostState);
    b.w().boolean(m.enabled);
    b.w().boolean(m.injecting);
    b.w().pad(2);
    return b.take();
}

bool decode(std::span<const std::byte> payload, HostState& out) {
    Reader r(payload);
    out.enabled = r.boolean();
    out.injecting = r.boolean();
    r.skip(2);
    return r.done();
}

// --- ActiveWindow ----------------------------------------------------------

std::vector<std::byte> encode(const ActiveWindow& m) {
    MessageBuilder b(MsgType::ActiveWindow);
    b.w().u32(m.pid);
    b.w().fixed_str(m.process, kProcessNameBytes);
    return b.take();
}

bool decode(std::span<const std::byte> payload, ActiveWindow& out) {
    Reader r(payload);
    out.pid = r.u32();
    out.process = r.fixed_str(kProcessNameBytes);
    return r.done();
}

// --- ViewportReq -------------------------------------------------------------

std::vector<std::byte> encode(const ViewportReq& m) {
    MessageBuilder b(MsgType::ViewportReq);
    b.w().i32(m.x);
    b.w().i32(m.y);
    b.w().i32(m.w);
    b.w().i32(m.h);
    b.w().u16(m.out_w);
    b.w().u16(m.out_h);
    b.w().u8(m.fps);
    b.w().u8(static_cast<std::uint8_t>(m.format));
    b.w().u8(m.tile);
    b.w().u8(m.flags);
    return b.take();
}

bool decode(std::span<const std::byte> payload, ViewportReq& out) {
    Reader r(payload);
    out.x = r.i32();
    out.y = r.i32();
    out.w = r.i32();
    out.h = r.i32();
    out.out_w = r.u16();
    out.out_h = r.u16();
    out.fps = r.u8();
    out.format = static_cast<FrameFormat>(r.u8());
    out.tile = r.u8();
    out.flags = r.u8();
    return r.done();
}

// --- FrameInfo ---------------------------------------------------------------

std::vector<std::byte> encode(const FrameInfo& m) {
    MessageBuilder b(MsgType::FrameInfo);
    b.w().u32(m.seq);
    b.w().i32(m.x);
    b.w().i32(m.y);
    b.w().i32(m.w);
    b.w().i32(m.h);
    b.w().u16(m.out_w);
    b.w().u16(m.out_h);
    b.w().u8(static_cast<std::uint8_t>(m.format));
    b.w().u8(m.tile);
    b.w().pad(2);
    b.w().u32(m.raw_bytes);
    b.w().u32(m.payload_bytes);
    for (const std::uint16_t tile : m.tiles) {
        b.w().u16(tile);
    }
    return b.take();
}

bool decode(std::span<const std::byte> payload, FrameInfo& out) {
    Reader r(payload);
    out.seq = r.u32();
    out.x = r.i32();
    out.y = r.i32();
    out.w = r.i32();
    out.h = r.i32();
    out.out_w = r.u16();
    out.out_h = r.u16();
    out.format = static_cast<FrameFormat>(r.u8());
    out.tile = r.u8();
    r.skip(2);
    out.raw_bytes = r.u32();
    out.payload_bytes = r.u32();

    // However many indices the rest of the payload holds. A trailing odd byte
    // means the sender and this reader disagree about the layout, which is
    // exactly what done() is for.
    out.tiles.clear();
    while (r.ok() && r.remaining() >= 2) {
        out.tiles.push_back(r.u16());
    }
    return r.done();
}

// --- FrameData ---------------------------------------------------------------

std::vector<std::byte> encode(const FrameData& m) {
    MessageBuilder b(MsgType::FrameData);
    b.w().u32(m.seq);
    b.w().u32(m.offset);
    b.w().bytes(m.bytes);
    return b.take();
}

bool decode(std::span<const std::byte> payload, FrameData& out) {
    Reader r(payload);
    out.seq = r.u32();
    out.offset = r.u32();
    const std::span<const std::byte> rest = r.rest_bytes();
    out.bytes.assign(rest.begin(), rest.end());
    return r.done();
}

// --- Key ---------------------------------------------------------------------

std::vector<std::byte> encode(const Key& m) {
    MessageBuilder b(MsgType::Key);
    b.w().u8(m.modifiers);
    b.w().u8(static_cast<std::uint8_t>(m.action));
    b.w().pad(2);
    b.w().fixed_str(m.key, kKeyNameBytes);
    return b.take();
}

bool decode(std::span<const std::byte> payload, Key& out) {
    Reader r(payload);
    out.modifiers = r.u8();
    out.action = static_cast<KeyAction>(r.u8());
    r.skip(2);
    out.key = r.fixed_str(kKeyNameBytes);
    return r.done();
}

// --- Log -------------------------------------------------------------------

std::vector<std::byte> encode(const LogMessage& m) {
    MessageBuilder b(MsgType::Log);
    b.w().u8(static_cast<std::uint8_t>(m.level));
    b.w().pad(3);
    b.w().raw_str(m.text);
    return b.take();
}

bool decode(std::span<const std::byte> payload, LogMessage& out) {
    Reader r(payload);
    out.level = static_cast<core::LogLevel>(r.u8());
    r.skip(3);
    out.text = r.rest_str();
    return r.done();
}

} // namespace digitiz::proto
