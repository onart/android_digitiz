#pragma once

// The guest side of the ADB reverse tunnel.
//
// `adb reverse tcp:27183 tcp:<host port>` makes the *device* listen on 27183,
// so from here it is an ordinary TCP client connecting to loopback. Requires
// android.permission.INTERNET even though nothing leaves the phone.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <thread>

#include <digitiz/proto/framer.hpp>

namespace digitiz::guest {

enum class LinkState : std::uint8_t {
    Stopped,
    Connecting,
    Connected,
};

class TcpTransport {
public:
    // Runs on the network thread.
    using MessageHandler = std::function<void(proto::MsgType, std::span<const std::byte>)>;
    using SessionHandler = std::function<void()>;

    ~TcpTransport();

    void set_handlers(MessageHandler on_message, SessionHandler on_connect,
                      SessionHandler on_disconnect);

    void start(std::uint16_t port);
    void stop();

    bool send(std::span<const std::byte> bytes);

    LinkState state() const noexcept { return state_.load(); }
    std::uint64_t rx_messages() const noexcept { return rx_messages_.load(); }
    std::uint64_t tx_messages() const noexcept { return tx_messages_.load(); }
    std::uint32_t connect_attempts() const noexcept { return attempts_.load(); }

private:
    void run();
    bool dial();
    void session_loop();
    void close_socket();
    void sleep_interruptibly(int milliseconds);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<LinkState> state_{LinkState::Stopped};

    std::atomic<std::uint64_t> rx_messages_{0};
    std::atomic<std::uint64_t> tx_messages_{0};
    std::atomic<std::uint32_t> attempts_{0};

    mutable std::mutex send_mutex_;
    int socket_ = -1;

    std::uint16_t port_ = 0;
    proto::Framer framer_;

    MessageHandler on_message_;
    SessionHandler on_connect_;
    SessionHandler on_disconnect_;
};

} // namespace digitiz::guest
