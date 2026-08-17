#pragma once

// ADB reverse TCP transport.
//
//   host listens on 127.0.0.1:<ephemeral>
//   adb reverse tcp:27183 tcp:<ephemeral>
//   guest connects to 127.0.0.1:27183 on the device
//
// which keeps PC = server and phone = client while running over USB.

#include <atomic>
#include <mutex>
#include <thread>

#include <digitiz/proto/framer.hpp>

#include "transport/AdbClient.hpp"
#include "transport/Transport.hpp"

namespace digitiz::host {

class AdbTransport final : public ITransport {
public:
    AdbTransport();
    ~AdbTransport() override;

    void set_handlers(MessageHandler on_message, SessionHandler on_connect,
                      SessionHandler on_disconnect) override;

    bool start() override;
    void stop() override;

    bool send(std::span<const std::byte> bytes) override;
    TransportStatus status() const override;
    void drop_session() override;

private:
    void run();                       // transport thread entry
    bool serve_one_session();         // returns false to back off before retrying
    void session_loop();              // recv until the socket closes
    void close_client();

    bool open_listener();
    void close_listener();

    void set_state(TransportState state, std::string detail);

    AdbClient adb_;
    proto::Framer framer_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    // Set while we are tearing a session down on purpose, so the resulting
    // socket error is reported as intent rather than as a fault.
    std::atomic<bool> dropping_{false};

    void sleep_interruptibly(int milliseconds);

    // Guards the client socket against a concurrent send() from the UI thread.
    mutable std::mutex send_mutex_;
    // SOCKET / int, held as uintptr_t so winsock stays out of this header.
    std::uintptr_t client_ = static_cast<std::uintptr_t>(-1);
    std::uintptr_t listener_ = static_cast<std::uintptr_t>(-1);

    // Counters are atomic rather than mutex-guarded so that send(), the RX
    // loop, and the UI can all touch them without a lock-ordering rule.
    std::atomic<std::uint64_t> rx_messages_{0};
    std::atomic<std::uint64_t> rx_bytes_{0};
    std::atomic<std::uint64_t> tx_messages_{0};
    std::atomic<std::uint64_t> tx_bytes_{0};
    std::atomic<std::uint64_t> resync_bytes_{0};
    std::atomic<std::uint64_t> sessions_{0};

    mutable std::mutex state_mutex_;
    TransportState state_ = TransportState::Stopped;
    std::string detail_;
    std::string serial_;
    std::string model_;
    int host_port_ = 0;

    MessageHandler on_message_;
    SessionHandler on_connect_;
    SessionHandler on_disconnect_;
};

} // namespace digitiz::host
