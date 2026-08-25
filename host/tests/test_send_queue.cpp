#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "transport/SendQueue.hpp"

using namespace digitiz::host;

namespace {

std::vector<std::byte> message(char tag, std::size_t size) {
    return std::vector<std::byte>(size, static_cast<std::byte>(tag));
}

// Drains the queue the way a sender would, `chunk` bytes at a time, and
// returns the byte stream that reached the socket.
std::string drain(SendQueue& q, std::size_t chunk = 4096) {
    std::string out;
    for (;;) {
        const std::span<const std::byte> span = q.pending(chunk);
        if (span.empty()) {
            break;
        }
        for (const std::byte b : span) {
            out.push_back(static_cast<char>(b));
        }
        q.consume(span.size());
    }
    return out;
}

std::size_t count_of(const std::string& s, char c) {
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), c));
}

} // namespace

TEST_CASE("an empty queue asks for nothing") {
    SendQueue q;
    CHECK(q.empty());
    CHECK(q.bulk_idle());
    CHECK(q.pending(1024).empty());
    CHECK(q.inversion_bytes() == 0);
    q.consume(100); // consuming nothing is not a crash
    CHECK(q.empty());
}

TEST_CASE("a pointer event overtakes frame data that is only queued") {
    SendQueue q;
    q.push_bulk(message('F', 8));
    q.push_bulk(message('G', 8));
    q.push_interactive(message('p', 4));

    // The frames were pushed first, and still the pointer goes first: nothing
    // had started going out yet.
    const std::string stream = drain(q);
    CHECK(stream == "ppppFFFFFFFFGGGGGGGG");
}

TEST_CASE("a message already going out is never cut in half") {
    SendQueue q;
    q.push_bulk(message('F', 100));

    // Start the frame, then a pointer event arrives mid-write.
    std::span<const std::byte> first = q.pending(40);
    CHECK(first.size() == 40);
    q.consume(40);
    q.push_interactive(message('p', 4));

    // Interrupting here would leave 40 bytes of a message in the stream with
    // something else spliced after it, and nothing downstream would parse
    // again. So the frame finishes first.
    CHECK(q.inversion_bytes() == 60);
    const std::string rest = drain(q);
    CHECK(rest == std::string(60, 'F') + "pppp");
}

TEST_CASE("the wait an interactive message can suffer is one message, not one queue") {
    SendQueue q;
    for (int i = 0; i < 20; ++i) {
        q.push_bulk(message('F', 16 * 1024));
    }

    // Twenty chunks are queued, but only the one in flight can delay anything.
    q.pending(4096);
    q.consume(4096);
    CHECK(q.bulk_bytes() == 19 * 16 * 1024);
    CHECK(q.inversion_bytes() == 16 * 1024 - 4096);

    q.push_interactive(message('p', 8));
    // Not 20 * 16 KiB. That is the whole design.
    CHECK(q.inversion_bytes() < 16 * 1024);
}

TEST_CASE("order is kept within a priority") {
    SendQueue q;
    q.push_interactive(message('a', 2));
    q.push_interactive(message('b', 2));
    q.push_bulk(message('X', 2));
    q.push_bulk(message('Y', 2));
    CHECK(drain(q) == "aabbXXYY");
}

TEST_CASE("a partial socket write resumes where it stopped") {
    SendQueue q;
    q.push_interactive(message('a', 10));

    // A socket that accepts three bytes at a time, which is what a full send
    // buffer looks like.
    std::string out;
    for (;;) {
        const std::span<const std::byte> span = q.pending(1024);
        if (span.empty()) {
            break;
        }
        const std::size_t took = std::min<std::size_t>(3, span.size());
        for (std::size_t i = 0; i < took; ++i) {
            out.push_back(static_cast<char>(span[i]));
        }
        q.consume(took);
    }
    CHECK(out == "aaaaaaaaaa");
}

TEST_CASE("a chunk limit is respected and never spans two messages") {
    SendQueue q;
    q.push_interactive(message('a', 10));
    q.push_interactive(message('b', 10));

    std::span<const std::byte> span = q.pending(64);
    // 64 was allowed, but the first message is 10 and the second must not be
    // glued onto it -- the caller is entitled to stop after any call.
    CHECK(span.size() == 10);
    q.consume(10);

    span = q.pending(4);
    CHECK(span.size() == 4);
}

TEST_CASE("bulk_idle says when the next frame may be queued") {
    SendQueue q;
    CHECK(q.bulk_idle());

    q.push_bulk(message('F', 20));
    CHECK_FALSE(q.bulk_idle());

    q.pending(8);
    q.consume(8);
    // Half sent still counts as in flight; queueing the next frame now would
    // put two frames in the pipe and defeat the point of dropping.
    CHECK_FALSE(q.bulk_idle());

    drain(q);
    CHECK(q.bulk_idle());

    // Interactive traffic has nothing to do with it.
    q.push_interactive(message('p', 4));
    CHECK(q.bulk_idle());
}

TEST_CASE("dropping queued frames leaves the half-written one alone") {
    SendQueue q;
    q.push_bulk(message('F', 20));
    q.push_bulk(message('G', 20));
    q.push_bulk(message('H', 20));

    q.pending(5);
    q.consume(5); // F is now in flight

    CHECK(q.drop_queued_bulk() == 2);
    CHECK(q.bulk_bytes() == 0);

    // F finishes, because truncating it would desynchronise everything after.
    const std::string rest = drain(q);
    CHECK(rest == std::string(15, 'F'));
}

TEST_CASE("dropping frames does not touch interactive traffic") {
    SendQueue q;
    q.push_interactive(message('p', 4));
    q.push_bulk(message('F', 8));
    q.drop_queued_bulk();
    CHECK(drain(q) == "pppp");
}

TEST_CASE("an oversize frame chunk is sent but counted") {
    SendQueue q;
    q.set_bulk_chunk_limit(1024);

    q.push_bulk(message('F', 500));
    CHECK(q.oversize_bulk() == 0);

    // Over the limit the latency bound above stops holding, so it is worth
    // knowing about rather than discovering as a stutter.
    q.push_bulk(message('G', 4096));
    CHECK(q.oversize_bulk() == 1);
    CHECK(count_of(drain(q), 'G') == 4096);
}

TEST_CASE("empty messages are ignored rather than becoming zero-length sends") {
    SendQueue q;
    q.push_interactive({});
    q.push_bulk({});
    CHECK(q.empty());
}

TEST_CASE("clearing drops everything, in flight included") {
    SendQueue q;
    q.push_interactive(message('a', 10));
    q.push_bulk(message('F', 10));
    q.pending(4);
    q.consume(4);

    // Used when the session ends: nothing queued for a socket that is gone.
    q.clear();
    CHECK(q.empty());
    CHECK(q.bulk_idle());
    CHECK(q.inversion_bytes() == 0);
    CHECK(q.pending(64).empty());
}

TEST_CASE("interleaved traffic comes out as whole messages in priority order") {
    SendQueue q;
    q.push_bulk(message('F', 6));
    q.push_interactive(message('a', 2));
    q.push_bulk(message('G', 6));
    q.push_interactive(message('b', 2));

    const std::string stream = drain(q, 3);
    // Both pointer events first, then the frames, and every message whole.
    CHECK(stream == "aabbFFFFFFGGGGGG");
}
