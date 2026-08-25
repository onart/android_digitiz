#pragma once

// Keeps a pointer event from waiting behind a screen frame.
//
// The two things this program sends have nothing in common. A pointer message
// is a few dozen bytes and is the entire point of the program; a batch of
// tiles is tens of kilobytes and can always be a frame later. Sent down one
// socket in the order they were produced, the second buries the first.
//
// The part that is easy to get wrong: a priority queue does not help if it
// sits below the socket. Once bytes are handed to the kernel they are
// committed, so writing a whole frame in one call puts every pointer event
// behind all of it no matter how the queue is ordered. This sits above the
// socket and hands out one message at a time.
//
// What it cannot do is interrupt a message already going out — the stream
// would no longer parse. So the worst an interactive message can wait is
// whatever is left of the message in flight, which is why frames are cut into
// chunks before they get here: at USB speeds 16 KiB is about 0.65 ms.
// inversion_bytes() reports that wait, so the claim can be checked rather
// than assumed.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace digitiz::host {

class SendQueue {
public:
    // Pointer, key, heartbeat, host state: small, and what the user is
    // actually waiting on.
    void push_interactive(std::vector<std::byte> message);

    // Screen tiles. Chunked by the producer, because only the producer can
    // split them somewhere the protocol survives.
    void push_bulk(std::vector<std::byte> message);

    // The next run of bytes for the socket, at most `max_bytes`. Never spans
    // two messages, so the caller may stop after any call and the next one
    // picks the highest-priority thing available then.
    std::span<const std::byte> pending(std::size_t max_bytes);

    // How many of those bytes the socket actually took.
    void consume(std::size_t bytes);

    bool empty() const noexcept;
    // True when no frame data is waiting or in flight, which is when the next
    // frame may be queued. Holding to one frame at a time is what makes a
    // dropped frame safe: the scheduler only marks tiles sent once they have
    // gone, so anything not sent simply comes round again.
    bool bulk_idle() const noexcept;

    std::size_t interactive_count() const noexcept { return interactive_.size(); }
    std::size_t bulk_count() const noexcept { return bulk_.size(); }
    std::size_t bulk_bytes() const noexcept { return bulk_bytes_; }

    // What an interactive message pushed right now would have to wait behind:
    // the remainder of the message in flight, and nothing else.
    std::size_t inversion_bytes() const noexcept;

    // Bulk messages pushed larger than this are still sent, but they break the
    // latency bound above, so they are counted and can be asserted on.
    void set_bulk_chunk_limit(std::size_t bytes) noexcept { bulk_limit_ = bytes; }
    std::uint64_t oversize_bulk() const noexcept { return oversize_bulk_; }

    // Drops frame data that has not started going out. Whatever is half
    // written is finished, because a truncated message would desynchronise the
    // stream for everything after it.
    std::size_t drop_queued_bulk();

    void clear();

private:
    bool in_flight() const noexcept { return offset_ < current_.size(); }
    void take_next();

    std::deque<std::vector<std::byte>> interactive_;
    std::deque<std::vector<std::byte>> bulk_;
    std::size_t bulk_bytes_ = 0;

    std::vector<std::byte> current_;
    std::size_t offset_ = 0;
    bool current_is_bulk_ = false;

    std::size_t bulk_limit_ = 64u * 1024u;
    std::uint64_t oversize_bulk_ = 0;
};

} // namespace digitiz::host
