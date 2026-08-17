#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include <digitiz/proto/framer.hpp>
#include <digitiz/proto/messages.hpp>

using namespace digitiz;
using namespace digitiz::proto;

namespace {

struct Captured {
    MsgType type{};
    std::uint8_t flags = 0;
    std::vector<std::byte> payload;
};

// The span handed to drain() dies with the callback, so copy it out.
std::vector<Captured> collect(Framer& f) {
    std::vector<Captured> got;
    f.drain([&](MsgType t, std::uint8_t flags, std::span<const std::byte> p) {
        got.push_back(Captured{t, flags, std::vector<std::byte>(p.begin(), p.end())});
    });
    return got;
}

void append(std::vector<std::byte>& dst, const std::vector<std::byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

} // namespace

TEST_CASE("a whole message in one push") {
    Framer f;
    f.push(encode(Ping{.t_send_us = 42}));

    const auto got = collect(f);
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == MsgType::Ping);

    Ping p;
    REQUIRE(decode(got[0].payload, p));
    CHECK(p.t_send_us == 42);

    CHECK(f.buffered() == 0);
    CHECK(f.resync_bytes() == 0);
}

TEST_CASE("nothing is emitted until the payload is complete") {
    const auto msg = encode(Pointer{.x = 7, .y = 9});

    Framer f;
    f.push(std::span<const std::byte>(msg).subspan(0, msg.size() - 1));
    CHECK(collect(f).empty());

    f.push(std::span<const std::byte>(msg).subspan(msg.size() - 1));

    const auto got = collect(f);
    REQUIRE(got.size() == 1);

    Pointer p;
    REQUIRE(decode(got[0].payload, p));
    CHECK(p.x == 7);
    CHECK(p.y == 9);
}

TEST_CASE("a message fed one byte at a time still arrives exactly once") {
    const auto msg = encode(Pointer{.x = -321, .y = 654});

    Framer f;
    std::vector<Captured> got;
    for (std::size_t i = 0; i < msg.size(); ++i) {
        f.push(std::span<const std::byte>(msg).subspan(i, 1));
        for (auto& c : collect(f)) {
            got.push_back(std::move(c));
        }
    }

    REQUIRE(got.size() == 1);
    Pointer p;
    REQUIRE(decode(got[0].payload, p));
    CHECK(p.x == -321);
    CHECK(p.y == 654);
    CHECK(f.resync_bytes() == 0);
}

TEST_CASE("several messages coalesced into one push come out in order") {
    std::vector<std::byte> stream;
    append(stream, encode(Ping{.t_send_us = 1}));
    append(stream, encode(Pointer{.x = 2}));
    append(stream, encode(Pong{.t_send_us = 1, .t_reply_us = 3}));

    Framer f;
    f.push(stream);

    const auto got = collect(f);
    REQUIRE(got.size() == 3);
    CHECK(got[0].type == MsgType::Ping);
    CHECK(got[1].type == MsgType::Pointer);
    CHECK(got[2].type == MsgType::Pong);
    CHECK(f.buffered() == 0);
}

TEST_CASE("messages split across arbitrary chunk boundaries") {
    std::vector<std::byte> stream;
    for (int i = 0; i < 20; ++i) {
        append(stream, encode(Pointer{.x = i, .y = -i}));
    }

    Framer f;
    std::vector<Captured> got;
    for (std::size_t off = 0; off < stream.size(); off += 7) {
        const std::size_t n = std::min<std::size_t>(7, stream.size() - off);
        f.push(std::span<const std::byte>(stream).subspan(off, n));
        for (auto& c : collect(f)) {
            got.push_back(std::move(c));
        }
    }

    REQUIRE(got.size() == 20);
    for (int i = 0; i < 20; ++i) {
        Pointer p;
        REQUIRE(decode(got[static_cast<std::size_t>(i)].payload, p));
        CHECK(p.x == i);
        CHECK(p.y == -i);
    }
    CHECK(f.resync_bytes() == 0);
}

TEST_CASE("leading garbage is skipped and counted") {
    std::vector<std::byte> stream{std::byte{0x00}, std::byte{0xFF}, std::byte{'D'}};
    append(stream, encode(Ping{.t_send_us = 5}));

    Framer f;
    f.push(stream);

    const auto got = collect(f);
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == MsgType::Ping);
    CHECK(f.resync_bytes() == 3); // 0x00, 0xFF, and the bare 'D'
}

TEST_CASE("an absurd payload length is treated as desync, not a 4 MiB wait") {
    std::vector<std::byte> stream{
        std::byte{'D'},  std::byte{'I'},  std::byte{0x10}, std::byte{0x00},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, // len = 4294967295
    };
    append(stream, encode(Ping{.t_send_us = 9}));

    Framer f;
    f.push(stream);

    const auto got = collect(f);
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == MsgType::Ping);
    CHECK(f.resync_bytes() > 0);
}

TEST_CASE("a lone 'D' at the tail is held, not dropped") {
    Framer f;
    f.push(std::vector<std::byte>{std::byte{'D'}});

    CHECK(collect(f).empty());
    CHECK(f.resync_bytes() == 0); // it may still turn into a valid header
    CHECK(f.buffered() == 1);

    // ...and it does.
    const auto msg = encode(Ping{.t_send_us = 77});
    f.push(std::span<const std::byte>(msg).subspan(1));

    const auto got = collect(f);
    REQUIRE(got.size() == 1);
    CHECK(f.resync_bytes() == 0);
}

TEST_CASE("a zero-length payload is a valid message") {
    const std::vector<std::byte> stream{
        std::byte{'D'},  std::byte{'I'},  std::byte{static_cast<std::uint8_t>(MsgType::Ping)},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},
    };

    Framer f;
    f.push(stream);

    const auto got = collect(f);
    REQUIRE(got.size() == 1);
    CHECK(got[0].payload.empty());
}

TEST_CASE("reset clears buffered bytes") {
    const auto msg = encode(Pointer{});
    Framer f;
    f.push(std::span<const std::byte>(msg).subspan(0, 4));
    CHECK(f.buffered() == 4);

    f.reset();
    CHECK(f.buffered() == 0);
    CHECK(collect(f).empty());
}
