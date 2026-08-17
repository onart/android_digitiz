#include "net/TcpTransport.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

constexpr std::size_t kRecvBufferBytes = 16 * 1024;
constexpr int kSelectSliceMs = 250;
constexpr int kMinBackoffMs = 250;
constexpr int kMaxBackoffMs = 4000;

// If the host process is killed, adb leaves the tunnel in place and this end
// never sees a close — the socket simply goes quiet. Without a timeout the
// guest would sit forever holding a connection to nothing, and would not
// reconnect when the host came back. The host sends a PING every second, so
// silence this long means it is gone.
constexpr int kReceiveTimeoutMs = 5000;

// adb accepts on the device side whether or not anything is listening at the
// PC end, so connect() succeeds and the socket dies a couple of seconds later.
// Such a session never really existed and must not reset the backoff, or the
// guest would spin reconnecting the whole time the host is down.
//
// The test is "did the host ever say anything", not a duration: adb's give-up
// delay lands right on top of any threshold worth picking, whereas a real host
// answers HELLO within milliseconds and a dead end never sends a byte.

} // namespace

TcpTransport::~TcpTransport() {
    stop();
}

void TcpTransport::set_handlers(MessageHandler on_message, SessionHandler on_connect,
                                SessionHandler on_disconnect) {
    on_message_ = std::move(on_message);
    on_connect_ = std::move(on_connect);
    on_disconnect_ = std::move(on_disconnect);
}

void TcpTransport::start(std::uint16_t port) {
    if (running_.exchange(true)) {
        return;
    }
    port_ = port;
    state_ = LinkState::Connecting;
    thread_ = std::thread([this] { run(); });
}

void TcpTransport::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard lock(send_mutex_);
        if (socket_ >= 0) {
            ::shutdown(socket_, SHUT_RDWR);
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    close_socket();
    state_ = LinkState::Stopped;
}

void TcpTransport::sleep_interruptibly(int milliseconds) {
    constexpr int kSlice = 100;
    for (int waited = 0; waited < milliseconds && running_; waited += kSlice) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSlice));
    }
}

void TcpTransport::run() {
    int backoff = kMinBackoffMs;

    while (running_) {
        state_ = LinkState::Connecting;

        if (!dial()) {
            // The host may simply not be running yet, which is normal: the app
            // can be launched by hand before the PC side exists.
            sleep_interruptibly(backoff);
            backoff = backoff * 2 < kMaxBackoffMs ? backoff * 2 : kMaxBackoffMs;
            continue;
        }

        framer_.reset();
        state_ = LinkState::Connected;
        DZ_DEBUG("socket connected through the reverse tunnel");

        const auto started = std::chrono::steady_clock::now();
        session_saw_data_ = false;

        if (on_connect_) {
            on_connect_();
        }

        session_loop();

        if (on_disconnect_) {
            on_disconnect_();
        }
        close_socket();

        const auto lived_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();

        if (session_saw_data_) {
            DZ_INFO("disconnected from host after %.1f s", static_cast<double>(lived_ms) / 1000.0);
            backoff = kMinBackoffMs;
            reported_unreachable_ = false;
        } else {
            // Say it once, then keep quiet: the host being off is a normal
            // state to sit in, not a stream of errors.
            if (!reported_unreachable_) {
                reported_unreachable_ = true;
                DZ_INFO("host is not reachable; retrying quietly in the background");
            }
            sleep_interruptibly(backoff);
            backoff = backoff * 2 < kMaxBackoffMs ? backoff * 2 : kMaxBackoffMs;
        }
    }

    state_ = LinkState::Stopped;
}

bool TcpTransport::dial() {
    ++attempts_;

    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        DZ_ERROR("socket() failed: %s", std::strerror(errno));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port_);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        // Connection refused just means adb has not mapped the port yet.
        ::close(fd);
        return false;
    }

    // Nagle would coalesce pointer events into latency that is nearly
    // impossible to attribute later. Not optional for a digitizer.
    int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        DZ_WARN("could not set TCP_NODELAY: %s", std::strerror(errno));
    }

    std::lock_guard lock(send_mutex_);
    socket_ = fd;
    return true;
}

void TcpTransport::session_loop() {
    std::vector<std::byte> buffer(kRecvBufferBytes);
    auto last_rx = std::chrono::steady_clock::now();

    while (running_) {
        int fd = -1;
        {
            std::lock_guard lock(send_mutex_);
            fd = socket_;
        }
        if (fd < 0) {
            return;
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);

        timeval tv{};
        tv.tv_sec = kSelectSliceMs / 1000;
        tv.tv_usec = (kSelectSliceMs % 1000) * 1000;

        const int ready = ::select(fd + 1, &read_set, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (ready == 0) {
            const auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - last_rx)
                                   .count();
            if (quiet >= kReceiveTimeoutMs) {
                DZ_WARN("host silent for %lld ms; assuming it is gone and reconnecting",
                        static_cast<long long>(quiet));
                return;
            }
            continue;
        }

        const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (got <= 0) {
            return;
        }
        last_rx = std::chrono::steady_clock::now();
        session_saw_data_ = true;

        framer_.push({buffer.data(), static_cast<std::size_t>(got)});
        framer_.drain([this](proto::MsgType type, std::uint8_t, std::span<const std::byte> payload) {
            ++rx_messages_;
            if (on_message_) {
                on_message_(type, payload);
            }
        });
    }
}

bool TcpTransport::send(std::span<const std::byte> bytes) {
    std::lock_guard lock(send_mutex_);
    if (socket_ < 0) {
        return false;
    }

    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(socket_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }

    ++tx_messages_;
    return true;
}

void TcpTransport::close_socket() {
    std::lock_guard lock(send_mutex_);
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

} // namespace digitiz::guest
