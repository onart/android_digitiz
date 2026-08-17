#pragma once

// Transport contract: a reliable ordered byte stream plus connection lifecycle.
//
// Nothing above this layer knows whether the bytes arrive over an ADB reverse
// tunnel or (later) an AOA bulk endpoint. Message framing happens here; message
// *meaning* does not.

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include <digitiz/proto/wire.hpp>

namespace digitiz::host {

// Fixed on the device side because the guest hard-codes it. The host side port
// is ephemeral, so two hosts never fight over a port.
inline constexpr int kDevicePort = 27183;
inline constexpr const char* kGuestPackage = "com.onart.digitiz";
inline constexpr const char* kGuestComponent = "com.onart.digitiz/.MainActivity";

// The guest keeps its settings here rather than the host keeping a copy. The
// host needs to know whether it may launch the app *while the app is not
// running*, so it cannot ask over the link — but adb can read the file. Must
// match kSettingsFileName on the guest.
inline constexpr const char* kGuestSettingsPath =
    "/sdcard/Android/data/com.onart.digitiz/files/settings.txt";

enum class TransportState : std::uint8_t {
    Stopped,
    NoAdb,
    WaitingForDevice,
    Unauthorized,     // device present, RSA prompt not accepted
    Preparing,        // setting up the reverse tunnel, launching the guest
    WaitingForClient, // tunnel up, guest has not connected yet
    Connected,
};

const char* to_string(TransportState state) noexcept;

struct TransportStatus {
    TransportState state = TransportState::Stopped;
    std::string device_serial;
    std::string device_model;
    std::string detail; // what to tell the user right now
    int host_port = 0;

    std::uint64_t rx_messages = 0;
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_messages = 0;
    std::uint64_t tx_bytes = 0;
    std::uint64_t resync_bytes = 0;
    std::uint64_t sessions = 0;
};

class ITransport {
public:
    // Called on the transport thread. Injection latency matters more than
    // thread-hopping convenience, so handlers run here rather than being
    // queued for the UI thread.
    using MessageHandler = std::function<void(proto::MsgType, std::span<const std::byte>)>;
    using SessionHandler = std::function<void()>;

    virtual ~ITransport() = default;

    virtual void set_handlers(MessageHandler on_message, SessionHandler on_connect,
                              SessionHandler on_disconnect) = 0;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual bool send(std::span<const std::byte> bytes) = 0;
    virtual TransportStatus status() const = 0;

    // Ends the current session without stopping the transport; it will go back
    // to waiting for the guest. Used when the heartbeat stops answering.
    virtual void drop_session() = 0;
};

} // namespace digitiz::host
