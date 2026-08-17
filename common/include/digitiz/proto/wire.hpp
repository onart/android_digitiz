#pragma once

// Wire format primitives. See docs/PROTOCOL.md.
//
// Everything is little-endian. The Writer/Reader below serialize field by field
// with explicit shifts rather than memcpy-ing structs, so the layout does not
// depend on how MSVC and Clang-Android happen to pad things.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace digitiz::proto {

// On-wire bytes are 'D','I'. Stored little-endian, so the u16 value is 0x4944.
inline constexpr std::uint16_t kMagic = 0x4944;
inline constexpr std::uint8_t kMagicByte0 = 'D';
inline constexpr std::uint8_t kMagicByte1 = 'I';

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 8;
inline constexpr std::uint32_t kMaxPayload = 4u * 1024u * 1024u;

enum class MsgType : std::uint8_t {
    Hello = 0x01,
    HelloAck = 0x02,
    Ping = 0x03,
    Pong = 0x04,

    Pointer = 0x10,
    HostState = 0x11,

    // Reserved for milestone 2. Numbered now so nothing gets renumbered later.
    ViewportReq = 0x20,
    FrameInfo = 0x21,
    FrameData = 0x22,
    ActiveWindow = 0x23,
    Key = 0x24,
    Wheel = 0x25,
    Smoothing = 0x26,

    Log = 0x7F,
};

const char* to_string(MsgType type) noexcept;

// ---------------------------------------------------------------------------

class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) noexcept : out_(&out) {}

    void u8(std::uint8_t v) { out_->push_back(static_cast<std::byte>(v)); }

    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }

    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }

    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }

    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }

    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }

    void boolean(bool v) { u8(v ? 1u : 0u); }

    void pad(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            u8(0);
        }
    }

    void bytes(std::span<const std::byte> b) { out_->insert(out_->end(), b.begin(), b.end()); }

    void raw_str(std::string_view s) {
        bytes({reinterpret_cast<const std::byte*>(s.data()), s.size()});
    }

    // Exactly `n` bytes: truncated if longer, NUL-padded if shorter.
    void fixed_str(std::string_view s, std::size_t n) {
        const std::size_t take = s.size() < n ? s.size() : n;
        raw_str(s.substr(0, take));
        pad(n - take);
    }

private:
    std::vector<std::byte>* out_;
};

// ---------------------------------------------------------------------------

// Reads latch a failure flag instead of throwing. Check ok() once at the end;
// every read after the first short read returns zero.
class Reader {
public:
    explicit Reader(std::span<const std::byte> in) noexcept : in_(in) {}

    std::uint8_t u8() {
        if (!need(1)) {
            return 0;
        }
        return static_cast<std::uint8_t>(in_[pos_++]);
    }

    std::uint16_t u16() {
        const std::uint16_t lo = u8();
        const std::uint16_t hi = u8();
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }

    std::uint32_t u32() {
        const std::uint32_t lo = u16();
        const std::uint32_t hi = u16();
        return lo | (hi << 16);
    }

    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        const std::uint64_t hi = u32();
        return lo | (hi << 32);
    }

    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    float f32() { return std::bit_cast<float>(u32()); }

    bool boolean() { return u8() != 0; }

    void skip(std::size_t n) {
        if (need(n)) {
            pos_ += n;
        }
    }

    // Reads exactly `n` bytes, returning the content up to the first NUL.
    std::string fixed_str(std::size_t n) {
        if (!need(n)) {
            return {};
        }
        const auto* p = reinterpret_cast<const char*>(in_.data() + pos_);
        pos_ += n;
        std::size_t len = 0;
        while (len < n && p[len] != '\0') {
            ++len;
        }
        return std::string(p, len);
    }

    std::string rest_str() {
        const std::size_t n = remaining();
        return fixed_str(n);
    }

    std::span<const std::byte> rest_bytes() {
        const auto s = in_.subspan(pos_);
        pos_ = in_.size();
        return s;
    }

    bool ok() const noexcept { return ok_; }

    std::size_t remaining() const noexcept { return ok_ ? in_.size() - pos_ : 0; }

    // True when the payload was fully consumed and nothing overran.
    bool done() const noexcept { return ok_ && pos_ == in_.size(); }

private:
    bool need(std::size_t n) noexcept {
        if (!ok_ || in_.size() - pos_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------

// Writes an 8-byte header with a placeholder length, then patches it in take().
class MessageBuilder {
public:
    explicit MessageBuilder(MsgType type, std::uint8_t flags = 0) : w_(buf_) {
        buf_.reserve(64);
        w_.u16(kMagic);
        w_.u8(static_cast<std::uint8_t>(type));
        w_.u8(flags);
        w_.u32(0); // payload_len, patched by take()
    }

    Writer& w() noexcept { return w_; }

    std::vector<std::byte> take() {
        const auto len = static_cast<std::uint32_t>(buf_.size() - kHeaderSize);
        buf_[4] = static_cast<std::byte>(len);
        buf_[5] = static_cast<std::byte>(len >> 8);
        buf_[6] = static_cast<std::byte>(len >> 16);
        buf_[7] = static_cast<std::byte>(len >> 24);
        return std::move(buf_);
    }

private:
    std::vector<std::byte> buf_;
    Writer w_;
};

} // namespace digitiz::proto
