#pragma once

// Recovers message boundaries from a byte stream.
//
// The transport (ADB reverse TCP now, AOA bulk later) guarantees a reliable
// ordered byte stream and nothing more: one recv() may hold three messages or
// half of one. Framer absorbs that.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <digitiz/proto/wire.hpp>

namespace digitiz::proto {

class Framer {
public:
    // Accumulate freshly received bytes.
    void push(std::span<const std::byte> data) { buf_.insert(buf_.end(), data.begin(), data.end()); }

    // Emits every complete message in order.
    //   fn(MsgType type, std::uint8_t flags, std::span<const std::byte> payload)
    // The payload span is valid only for the duration of that call — copy out
    // anything you intend to keep.
    template <class F>
    void drain(F&& fn) {
        for (;;) {
            if (!seek_magic()) {
                break;
            }
            if (buffered() < kHeaderSize) {
                break;
            }

            const auto type = static_cast<std::uint8_t>(buf_[read_ + 2]);
            const auto flags = static_cast<std::uint8_t>(buf_[read_ + 3]);
            const std::uint32_t len = u32_at(read_ + 4);

            // A length this large means we are not actually on a header.
            if (len > kMaxPayload) {
                drop_byte();
                continue;
            }
            if (buffered() < kHeaderSize + len) {
                break; // wait for the rest
            }

            fn(static_cast<MsgType>(type), flags,
               std::span<const std::byte>(buf_.data() + read_ + kHeaderSize, len));
            read_ += kHeaderSize + len;
        }
        compact();
    }

    // Bytes discarded while resynchronizing. Non-zero means a bug somewhere —
    // the transport is reliable, so this should stay at 0 forever.
    std::uint64_t resync_bytes() const noexcept { return resync_bytes_; }

    std::size_t buffered() const noexcept { return buf_.size() - read_; }

    void reset() noexcept {
        buf_.clear();
        read_ = 0;
    }

private:
    static constexpr std::size_t kCompactThreshold = 64u * 1024u;

    std::uint32_t u32_at(std::size_t i) const noexcept {
        return static_cast<std::uint32_t>(buf_[i]) | (static_cast<std::uint32_t>(buf_[i + 1]) << 8) |
               (static_cast<std::uint32_t>(buf_[i + 2]) << 16) |
               (static_cast<std::uint32_t>(buf_[i + 3]) << 24);
    }

    void drop_byte() noexcept {
        ++read_;
        ++resync_bytes_;
    }

    // Advances until buf_[read_] could begin a valid magic.
    // Returns false when more data is needed to decide.
    bool seek_magic() noexcept {
        for (;;) {
            if (buffered() < 1) {
                return false;
            }
            if (static_cast<std::uint8_t>(buf_[read_]) != kMagicByte0) {
                drop_byte();
                continue;
            }
            if (buffered() < 2) {
                return false; // 'D' might yet be followed by 'I'
            }
            if (static_cast<std::uint8_t>(buf_[read_ + 1]) != kMagicByte1) {
                drop_byte();
                continue;
            }
            return true;
        }
    }

    void compact() {
        if (read_ == 0) {
            return;
        }
        if (read_ == buf_.size()) {
            buf_.clear();
            read_ = 0;
        } else if (read_ >= kCompactThreshold) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(read_));
            read_ = 0;
        }
    }

    std::vector<std::byte> buf_;
    std::size_t read_ = 0;
    std::uint64_t resync_bytes_ = 0;
};

} // namespace digitiz::proto
