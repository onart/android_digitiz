#include <doctest/doctest.h>

#include <digitiz/proto/messages.hpp>

using namespace digitiz;
using namespace digitiz::proto;

namespace {

std::span<const std::byte> payload_of(const std::vector<std::byte>& msg) {
    return std::span<const std::byte>(msg).subspan(kHeaderSize);
}

std::uint8_t byte_at(const std::vector<std::byte>& msg, std::size_t i) {
    return static_cast<std::uint8_t>(msg[i]);
}

} // namespace

TEST_CASE("header is 8 bytes and starts with the ASCII magic 'D','I'") {
    const auto msg = encode(Ping{.t_send_us = 1});

    REQUIRE(msg.size() == kHeaderSize + 8);
    CHECK(byte_at(msg, 0) == 'D');
    CHECK(byte_at(msg, 1) == 'I');
    CHECK(byte_at(msg, 2) == static_cast<std::uint8_t>(MsgType::Ping));
    CHECK(byte_at(msg, 3) == 0); // flags

    // payload_len, little-endian
    CHECK(byte_at(msg, 4) == 8);
    CHECK(byte_at(msg, 5) == 0);
    CHECK(byte_at(msg, 6) == 0);
    CHECK(byte_at(msg, 7) == 0);
}

TEST_CASE("integers are little-endian on the wire") {
    const auto msg = encode(Ping{.t_send_us = 0x0102030405060708ull});
    const auto p = payload_of(msg);

    CHECK(static_cast<std::uint8_t>(p[0]) == 0x08);
    CHECK(static_cast<std::uint8_t>(p[7]) == 0x01);
}

TEST_CASE("Hello round-trips") {
    const Hello in{
        .proto_ver = kProtocolVersion,
        .screen_w = 1080,
        .screen_h = 2400,
        .density = 2.75f,
        .device = "Pixel 8",
    };

    Hello out;
    REQUIRE(decode(payload_of(encode(in)), out));

    CHECK(out.proto_ver == in.proto_ver);
    CHECK(out.screen_w == in.screen_w);
    CHECK(out.screen_h == in.screen_h);
    CHECK(out.density == doctest::Approx(in.density));
    CHECK(out.device == in.device);
}

TEST_CASE("Hello device name is truncated, not overflowed") {
    Hello in;
    in.device = std::string(200, 'x');

    const auto msg = encode(in);
    CHECK(msg.size() == kHeaderSize + 16 + kDeviceNameBytes);

    Hello out;
    REQUIRE(decode(payload_of(msg), out));
    CHECK(out.device.size() == kDeviceNameBytes);
}

TEST_CASE("HelloAck round-trips with a negative-origin multi-monitor layout") {
    const HelloAck in{
        .proto_ver = kProtocolVersion,
        .host_os = HostOs::Windows,
        .vx = -1920,
        .vy = -120,
        .vw = 3840,
        .vh = 1200,
        .monitors =
            {
                Monitor{.x = -1920, .y = -120, .w = 1920, .h = 1080, .dpi = 96, .primary = false},
                Monitor{.x = 0, .y = 0, .w = 1920, .h = 1200, .dpi = 144, .primary = true},
            },
    };

    HelloAck out;
    REQUIRE(decode(payload_of(encode(in)), out));

    CHECK(out.host_os == in.host_os);
    CHECK(out.vx == -1920);
    CHECK(out.vy == -120);
    CHECK(out.vw == in.vw);
    CHECK(out.vh == in.vh);

    REQUIRE(out.monitors.size() == 2);
    CHECK(out.monitors[0].x == -1920);
    CHECK(out.monitors[0].primary == false);
    CHECK(out.monitors[1].dpi == 144);
    CHECK(out.monitors[1].primary == true);
}

TEST_CASE("HelloAck round-trips with no monitors") {
    HelloAck out;
    REQUIRE(decode(payload_of(encode(HelloAck{})), out));
    CHECK(out.monitors.empty());
}

TEST_CASE("Pointer round-trips including negative coordinates") {
    const Pointer in{
        .t_us = 123456789,
        .x = -1500,
        .y = -37,
        .action = PointerAction::Cancel,
        .button = MouseButton::Right,
        .pointer_id = 0,
        .flags = 0,
        .pressure = 0.625f,
    };

    const auto msg = encode(in);
    CHECK(msg.size() == kHeaderSize + 24); // documented payload size

    Pointer out;
    REQUIRE(decode(payload_of(msg), out));

    CHECK(out.t_us == in.t_us);
    CHECK(out.x == -1500);
    CHECK(out.y == -37);
    CHECK(out.action == PointerAction::Cancel);
    CHECK(out.button == MouseButton::Right);
    CHECK(out.pressure == doctest::Approx(0.625f));
}

TEST_CASE("Pong and HostState round-trip") {
    Pong pong_out;
    REQUIRE(decode(payload_of(encode(Pong{.t_send_us = 11, .t_reply_us = 22})), pong_out));
    CHECK(pong_out.t_send_us == 11);
    CHECK(pong_out.t_reply_us == 22);

    HostState state_out;
    REQUIRE(decode(payload_of(encode(HostState{.enabled = true, .injecting = false})), state_out));
    CHECK(state_out.enabled);
    CHECK_FALSE(state_out.injecting);
}

TEST_CASE("ActiveWindow round-trips") {
    ActiveWindow out;
    REQUIRE(decode(payload_of(encode(ActiveWindow{.pid = 4242, .process = "krita.exe"})), out));
    CHECK(out.pid == 4242);
    CHECK(out.process == "krita.exe");
}

TEST_CASE("ActiveWindow carries an unidentified window as an empty name") {
    ActiveWindow out;
    REQUIRE(decode(payload_of(encode(ActiveWindow{.pid = 0, .process = ""})), out));
    CHECK(out.pid == 0);
    CHECK(out.process.empty());
}

TEST_CASE("ActiveWindow truncates an over-long process name") {
    // Longer than the field, which the wire has to survive rather than
    // overrun. Windows path components can reach 255 characters.
    const std::string long_name(kProcessNameBytes + 20, 'a');

    ActiveWindow out;
    REQUIRE(decode(payload_of(encode(ActiveWindow{.pid = 1, .process = long_name})), out));
    CHECK(out.process.size() == kProcessNameBytes);
}

TEST_CASE("Key round-trips a modified shortcut") {
    Key out;
    REQUIRE(decode(payload_of(encode(Key{.modifiers = kModCtrl | kModShift,
                                         .action = KeyAction::Press,
                                         .key = "s"})),
                   out));
    CHECK(out.modifiers == (kModCtrl | kModShift));
    CHECK(out.action == KeyAction::Press);
    CHECK(out.key == "s");
}

TEST_CASE("Key round-trips a named key with no modifiers") {
    Key out;
    REQUIRE(decode(payload_of(encode(Key{.key = "escape"})), out));
    CHECK(out.modifiers == 0);
    CHECK(out.key == "escape");
}

TEST_CASE("LogMessage carries a variable-length body") {
    const LogMessage in{.level = core::LogLevel::Warn, .text = "reverse tunnel dropped"};

    LogMessage out;
    REQUIRE(decode(payload_of(encode(in)), out));

    CHECK(out.level == core::LogLevel::Warn);
    CHECK(out.text == in.text);
}

TEST_CASE("LogMessage handles an empty body") {
    LogMessage out;
    REQUIRE(decode(payload_of(encode(LogMessage{.level = core::LogLevel::Trace, .text = ""})), out));
    CHECK(out.text.empty());
}

TEST_CASE("decode rejects a truncated payload") {
    const auto msg = encode(Pointer{});
    const auto full = payload_of(msg);

    Pointer out;
    CHECK_FALSE(decode(full.subspan(0, full.size() - 1), out));
}

TEST_CASE("decode rejects trailing garbage") {
    auto msg = encode(Pointer{});
    msg.push_back(std::byte{0xAB});

    Pointer out;
    CHECK_FALSE(decode(payload_of(msg), out));
}

// --- screen transfer ---------------------------------------------------------

TEST_CASE("ViewportReq round-trips, negative origin included") {
    const ViewportReq in{.x = -1920,
                         .y = -40,
                         .w = 1280,
                         .h = 720,
                         .out_w = 640,
                         .out_h = 360,
                         .fps = 30,
                         .format = FrameFormat::Etc2Rgb8,
                         .tile = 64,
                         .flags = kViewportCursor};

    ViewportReq out;
    REQUIRE(decode(payload_of(encode(in)), out));
    CHECK(out.x == -1920);
    CHECK(out.y == -40);
    CHECK(out.w == 1280);
    CHECK(out.out_h == 360);
    CHECK(out.fps == 30);
    CHECK(out.format == FrameFormat::Etc2Rgb8);
    CHECK(out.tile == 64);
    CHECK((out.flags & kViewportCursor) != 0);
}

TEST_CASE("a stopped stream is still a request, not an absence of one") {
    ViewportReq out;
    REQUIRE(decode(payload_of(encode(ViewportReq{.w = 800, .h = 600, .fps = 0})), out));
    CHECK(out.fps == 0);
    CHECK(out.w == 800);
}

TEST_CASE("FrameInfo carries its tile list") {
    FrameInfo in;
    in.seq = 7;
    in.x = 100;
    in.y = 200;
    in.w = 640;
    in.h = 480;
    in.out_w = 640;
    in.out_h = 480;
    in.format = FrameFormat::Etc2Rgb8;
    in.tile = 64;
    in.raw_bytes = 8192;
    in.payload_bytes = 1234;
    in.tiles = {0, 5, 9, 60000};

    FrameInfo out;
    REQUIRE(decode(payload_of(encode(in)), out));
    CHECK(out.seq == 7);
    CHECK(out.tile == 64);
    CHECK(out.raw_bytes == 8192);
    CHECK(out.payload_bytes == 1234);
    CHECK(out.tiles == std::vector<std::uint16_t>{0, 5, 9, 60000});
}

TEST_CASE("a FrameInfo with no tiles is legal and means nothing changed") {
    FrameInfo out;
    REQUIRE(decode(payload_of(encode(FrameInfo{})), out));
    CHECK(out.tiles.empty());
}

TEST_CASE("FrameData carries its slice and where it belongs") {
    FrameData in;
    in.seq = 9;
    in.offset = 16384;
    in.bytes = {std::byte{1}, std::byte{2}, std::byte{0xFF}};

    FrameData out;
    REQUIRE(decode(payload_of(encode(in)), out));
    CHECK(out.seq == 9);
    CHECK(out.offset == 16384);
    REQUIRE(out.bytes.size() == 3);
    CHECK(out.bytes[2] == std::byte{0xFF});
}

TEST_CASE("an odd trailing byte in FrameInfo is a disagreement, not a tile") {
    auto msg = encode(FrameInfo{});
    msg.push_back(std::byte{0x01});

    FrameInfo out;
    CHECK_FALSE(decode(payload_of(msg), out));
}
