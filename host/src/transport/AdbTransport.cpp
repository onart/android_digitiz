#include "transport/AdbTransport.hpp"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define DZ_CLOSESOCK ::closesocket
#define DZ_LAST_SOCK_ERROR ::WSAGetLastError()
using sockopt_ptr = const char*;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define DZ_CLOSESOCK ::close
#define DZ_LAST_SOCK_ERROR errno
using sockopt_ptr = const void*;
#endif

#include <digitiz/core/log.hpp>

namespace digitiz::host {

namespace {

constexpr std::uintptr_t kNoSocket = static_cast<std::uintptr_t>(-1);
constexpr std::size_t kRecvBufferBytes = 16 * 1024;
constexpr int kAcceptTimeoutMs = 15000;
constexpr int kSelectSliceMs = 250;
constexpr int kRetryBackoffMs = 1000;

sock_t as_sock(std::uintptr_t handle) {
    return static_cast<sock_t>(handle);
}

bool valid(std::uintptr_t handle) {
    return handle != kNoSocket;
}

// Waits until the socket is readable. -1 error, 0 timeout, 1 readable.
int wait_readable(sock_t s, int timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(s, &read_set);

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
    return ::select(0, &read_set, nullptr, nullptr, &tv);
#else
    return ::select(s + 1, &read_set, nullptr, nullptr, &tv);
#endif
}

} // namespace

const char* to_string(TransportState state) noexcept {
    switch (state) {
    case TransportState::Stopped:
        return "Stopped";
    case TransportState::NoAdb:
        return "No adb";
    case TransportState::WaitingForDevice:
        return "Waiting for device";
    case TransportState::Unauthorized:
        return "Unauthorized";
    case TransportState::Preparing:
        return "Preparing";
    case TransportState::WaitingForClient:
        return "Waiting for guest";
    case TransportState::Connected:
        return "Connected";
    }
    return "?";
}

// ---------------------------------------------------------------------------

AdbTransport::AdbTransport() {
#ifdef _WIN32
    WSADATA wsa{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        DZ_ERROR("WSAStartup failed: %d", rc);
    }
#endif
}

AdbTransport::~AdbTransport() {
    stop();
#ifdef _WIN32
    ::WSACleanup();
#endif
}

void AdbTransport::set_handlers(MessageHandler on_message, SessionHandler on_connect,
                                SessionHandler on_disconnect) {
    on_message_ = std::move(on_message);
    on_connect_ = std::move(on_connect);
    on_disconnect_ = std::move(on_disconnect);
}

bool AdbTransport::start() {
    if (running_.exchange(true)) {
        return true;
    }
    set_state(TransportState::WaitingForDevice, "starting");
    thread_ = std::thread([this] { run(); });
    sender_ = std::thread([this] { send_loop(); });
    return true;
}

void AdbTransport::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // Break the blocking recv so the thread notices promptly.
    {
        std::lock_guard lock(send_mutex_);
        if (valid(client_)) {
#ifdef _WIN32
            ::shutdown(as_sock(client_), SD_BOTH);
#else
            ::shutdown(as_sock(client_), SHUT_RDWR);
#endif
        }
    }

    // Wake the sender out of its wait before joining either thread.
    send_cv_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
    if (sender_.joinable()) {
        sender_.join();
    }

    close_client();
    close_listener();
    set_state(TransportState::Stopped, {});
}

void AdbTransport::sleep_interruptibly(int milliseconds) {
    constexpr int kSlice = 100;
    for (int waited = 0; waited < milliseconds && running_; waited += kSlice) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSlice));
    }
}

void AdbTransport::run() {
    DZ_INFO("transport thread started");
    while (running_) {
        if (!serve_one_session()) {
            sleep_interruptibly(kRetryBackoffMs);
        }
    }
    DZ_INFO("transport thread stopped");
}

bool AdbTransport::serve_one_session() {
    if (!adb_.ready() && !adb_.init()) {
        set_state(TransportState::NoAdb, "adb not found — put it on PATH or set ANDROID_HOME");
        return false;
    }

    // --- pick a device -----------------------------------------------------
    const std::vector<AdbDevice> devices = adb_.devices();

    const AdbDevice* chosen = nullptr;
    bool saw_unauthorized = false;
    for (const AdbDevice& d : devices) {
        if (d.usable() && chosen == nullptr) {
            chosen = &d;
        }
        if (d.state == "unauthorized") {
            saw_unauthorized = true;
        }
    }

    if (chosen == nullptr) {
        // The device is gone, so the next one that shows up gets a fresh
        // launch — including the same phone after a replug or reboot.
        launched_serial_.clear();
        if (saw_unauthorized) {
            set_state(TransportState::Unauthorized,
                      "accept the USB debugging prompt on the phone");
        } else {
            set_state(TransportState::WaitingForDevice,
                      "connect the phone over USB with USB debugging on");
        }
        return false;
    }

    const std::string serial = chosen->serial;
    const std::string model = chosen->model;

    {
        std::lock_guard lock(state_mutex_);
        serial_ = serial;
        model_ = model;
    }
    set_state(TransportState::Preparing, "setting up the reverse tunnel");

    // --- tunnel ------------------------------------------------------------
    if (!open_listener()) {
        return false;
    }

    int port = 0;
    {
        std::lock_guard lock(state_mutex_);
        port = host_port_;
    }

    // A previous host that crashed or was killed leaves its mapping behind,
    // pointing at a port nobody is listening on. Clear it first so the tunnel
    // state is deterministic rather than inherited.
    adb_.reverse_remove(serial, kDevicePort);

    if (!adb_.reverse(serial, kDevicePort, port)) {
        close_listener();
        return false;
    }

    // Launch the guest at most once per device attachment, and never while it
    // is already running.
    //
    // This loop retries roughly every 16 seconds whenever nothing is
    // connected. Calling `am start` each time round would drag the app to the
    // foreground while the user is doing something else, and would resurrect
    // it seconds after they deliberately closed it. Once it is running it
    // reconnects on its own, so there is nothing to launch.
    if (launched_serial_ != serial) {
        if (adb_.is_app_running(serial, kGuestPackage)) {
            DZ_DEBUG("guest already running; waiting for it to connect");
        } else if (!adb_.read_guest_flag(serial, kGuestSettingsPath, "auto_launch", true)) {
            // The phone owns this decision, and it is off. Keep the tunnel up
            // so the app connects the moment the user opens it themselves.
            DZ_INFO("guest has auto-launch disabled; waiting for it to be opened");
        } else {
            adb_.start_activity(serial, kGuestComponent);
        }
        // Set either way: a launch that failed (app not installed) should be
        // reported once per attachment, not every retry.
        launched_serial_ = serial;
    }

    set_state(TransportState::WaitingForClient, "waiting for the guest to connect");

    // --- accept ------------------------------------------------------------
    std::uintptr_t accepted = kNoSocket;
    for (int waited = 0; waited < kAcceptTimeoutMs && running_; waited += kSelectSliceMs) {
        const int ready = wait_readable(as_sock(listener_), kSelectSliceMs);
        if (ready < 0) {
            DZ_ERROR("select on listener failed: %d", DZ_LAST_SOCK_ERROR);
            break;
        }
        if (ready == 0) {
            continue;
        }
        const sock_t s = ::accept(as_sock(listener_), nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            DZ_ERROR("accept failed: %d", DZ_LAST_SOCK_ERROR);
            break;
        }
        accepted = static_cast<std::uintptr_t>(s);
        break;
    }

    if (!valid(accepted)) {
        adb_.reverse_remove(serial, kDevicePort);
        close_listener();
        return false;
    }

    // Nagle would batch small pointer events and add latency that is very hard
    // to attribute later. This is not optional for a digitizer.
    int one = 1;
    if (::setsockopt(as_sock(accepted), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<sockopt_ptr>(&one), sizeof(one)) != 0) {
        DZ_WARN("could not set TCP_NODELAY: %d", DZ_LAST_SOCK_ERROR);
    }

    {
        std::lock_guard lock(send_mutex_);
        client_ = accepted;
    }

    framer_.reset();
    dropping_ = false;
    ++sessions_;
    set_state(TransportState::Connected, model.empty() ? serial : model + " (" + serial + ")");
    DZ_INFO("guest connected: %s %s", serial.c_str(), model.c_str());

    if (on_connect_) {
        on_connect_();
    }

    session_loop();

    if (on_disconnect_) {
        on_disconnect_();
    }

    DZ_INFO("guest disconnected");
    close_client();
    adb_.reverse_remove(serial, kDevicePort);
    close_listener();
    return true;
}

void AdbTransport::session_loop() {
    std::vector<std::byte> buffer(kRecvBufferBytes);

    while (running_) {
        sock_t client = INVALID_SOCKET;
        {
            std::lock_guard lock(send_mutex_);
            if (!valid(client_)) {
                return;
            }
            client = as_sock(client_);
        }

        const int ready = wait_readable(client, kSelectSliceMs);
        if (ready < 0) {
            if (!dropping_ && running_) {
                DZ_WARN("select on client failed: %d", DZ_LAST_SOCK_ERROR);
            }
            return;
        }
        if (ready == 0) {
            continue;
        }

        const int got = ::recv(client, reinterpret_cast<char*>(buffer.data()),
                               static_cast<int>(buffer.size()), 0);
        if (got == 0) {
            DZ_INFO("guest closed the connection");
            return;
        }
        if (got < 0) {
            // A socket error right after we shut the socket down ourselves is
            // the expected consequence, not news.
            if (dropping_) {
                DZ_INFO("session dropped by host");
            } else if (running_) {
                DZ_WARN("recv failed: %d", DZ_LAST_SOCK_ERROR);
            }
            return;
        }

        rx_bytes_ += static_cast<std::uint64_t>(got);
        framer_.push({buffer.data(), static_cast<std::size_t>(got)});

        framer_.drain([this](proto::MsgType type, std::uint8_t, std::span<const std::byte> payload) {
            ++rx_messages_;
            if (on_message_) {
                on_message_(type, payload);
            }
        });

        const std::uint64_t resyncs = framer_.resync_bytes();
        if (resyncs != resync_bytes_.exchange(resyncs)) {
            DZ_WARN("framer resynchronized: %llu byte(s) discarded in total",
                    static_cast<unsigned long long>(resyncs));
        }
    }
}

bool AdbTransport::send(std::span<const std::byte> bytes, SendPriority priority) {
    if (bytes.empty()) {
        return true;
    }
    {
        std::lock_guard lock(send_mutex_);
        if (!valid(client_)) {
            return false;
        }
        std::vector<std::byte> message(bytes.begin(), bytes.end());
        if (priority == SendPriority::Bulk) {
            queue_.push_bulk(std::move(message));
        } else {
            queue_.push_interactive(std::move(message));
        }
        ++tx_messages_;
        tx_bytes_ += bytes.size();
    }
    send_cv_.notify_one();

    // True means queued, not delivered. Nothing waits on delivery: callers
    // that care learn about a dead link from the heartbeat, and blocking here
    // is the thing this queue exists to stop.
    return true;
}

void AdbTransport::send_loop() {
    // The most that reaches one ::send call. A blocking write of more than
    // this is a stall nothing can preempt, so this is also the ceiling on how
    // long the socket can be busy with a single call.
    constexpr std::size_t kWriteChunk = 16u * 1024u;
    std::vector<std::byte> chunk;

    while (running_) {
        sock_t client{};
        {
            std::unique_lock lock(send_mutex_);
            send_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !running_ || (!queue_.empty() && valid(client_));
            });
            if (!running_) {
                return;
            }
            if (!valid(client_)) {
                // No session. Anything queued belongs to a socket that is
                // gone, and keeping it would deliver it to whoever connects
                // next.
                queue_.clear();
                continue;
            }

            const std::span<const std::byte> pending = queue_.pending(kWriteChunk);
            if (pending.empty()) {
                continue;
            }
            // Copied so the socket call happens with the lock released. It is
            // one chunk at most, and holding the lock across a blocking write
            // would stall the pointer path this arrangement exists to protect.
            chunk.assign(pending.begin(), pending.end());
            client = as_sock(client_);
        }

        const int n = ::send(client, reinterpret_cast<const char*>(chunk.data()),
                             static_cast<int>(chunk.size()), 0);
        if (n <= 0) {
            if (!dropping_) {
                DZ_WARN("send failed: %d", DZ_LAST_SOCK_ERROR);
            }
            std::lock_guard lock(send_mutex_);
            queue_.clear();
            continue;
        }

        std::lock_guard lock(send_mutex_);
        queue_.consume(static_cast<std::size_t>(n));
    }
}

bool AdbTransport::bulk_idle() const {
    std::lock_guard lock(send_mutex_);
    return queue_.bulk_idle();
}

TransportStatus AdbTransport::status() const {
    TransportStatus out;
    {
        std::lock_guard lock(state_mutex_);
        out.state = state_;
        out.detail = detail_;
        out.device_serial = serial_;
        out.device_model = model_;
        out.host_port = host_port_;
    }
    out.rx_messages = rx_messages_;
    out.rx_bytes = rx_bytes_;
    out.tx_messages = tx_messages_;
    out.tx_bytes = tx_bytes_;
    out.resync_bytes = resync_bytes_;
    out.sessions = sessions_;
    return out;
}

void AdbTransport::set_state(TransportState state, std::string detail) {
    bool changed = false;
    std::string snapshot;
    {
        std::lock_guard lock(state_mutex_);
        changed = state_ != state || detail_ != detail;
        state_ = state;
        detail_ = std::move(detail);
        snapshot = detail_;
    }
    // Log outside the lock: the sink takes its own mutex.
    if (changed) {
        DZ_INFO("transport: %s%s%s", to_string(state), snapshot.empty() ? "" : " - ",
                snapshot.c_str());
    }
}

void AdbTransport::drop_session() {
    std::lock_guard lock(send_mutex_);
    if (!valid(client_)) {
        return;
    }
    dropping_ = true;
    // Shutting the socket down makes the blocking recv return, so the session
    // loop unwinds through its normal disconnect path.
#ifdef _WIN32
    ::shutdown(as_sock(client_), SD_BOTH);
#else
    ::shutdown(as_sock(client_), SHUT_RDWR);
#endif
}

// ---------------------------------------------------------------------------

bool AdbTransport::open_listener() {
    close_listener();

    const sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        DZ_ERROR("socket() failed: %d", DZ_LAST_SOCK_ERROR);
        return false;
    }

    // Bind to an ephemeral port on loopback only: adb connects from localhost,
    // and port 0 means two hosts can never collide.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        DZ_ERROR("bind() failed: %d", DZ_LAST_SOCK_ERROR);
        DZ_CLOSESOCK(s);
        return false;
    }
    if (::listen(s, 1) != 0) {
        DZ_ERROR("listen() failed: %d", DZ_LAST_SOCK_ERROR);
        DZ_CLOSESOCK(s);
        return false;
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        DZ_ERROR("getsockname() failed: %d", DZ_LAST_SOCK_ERROR);
        DZ_CLOSESOCK(s);
        return false;
    }

    listener_ = static_cast<std::uintptr_t>(s);
    {
        std::lock_guard lock(state_mutex_);
        host_port_ = ::ntohs(bound.sin_port);
    }
    return true;
}

void AdbTransport::close_listener() {
    if (valid(listener_)) {
        DZ_CLOSESOCK(as_sock(listener_));
        listener_ = kNoSocket;
    }
}

void AdbTransport::close_client() {
    std::lock_guard lock(send_mutex_);
    if (valid(client_)) {
        DZ_CLOSESOCK(as_sock(client_));
        client_ = kNoSocket;
    }
}

} // namespace digitiz::host
