#pragma once

// Thin wrapper over the `adb` CLI: locating it, listing devices, setting up the
// reverse tunnel, launching the guest.

#include <string>
#include <vector>

#include "platform/Process.hpp"

namespace digitiz::host {

struct AdbDevice {
    std::string serial;
    std::string state; // "device", "unauthorized", "offline", ...
    std::string model;

    bool usable() const noexcept { return state == "device"; }
};

class AdbClient {
public:
    // Locates adb and reads its version. Safe to call repeatedly.
    bool init();

    bool ready() const noexcept { return !path_.empty(); }
    const std::string& path() const noexcept { return path_; }
    const std::string& version() const noexcept { return version_; }

    std::vector<AdbDevice> devices();

    // Makes the *device* listen on device_port and tunnel to the host's
    // host_port. The guest then just connects to 127.0.0.1:device_port.
    bool reverse(const std::string& serial, int device_port, int host_port);
    bool reverse_remove(const std::string& serial, int device_port);

    bool start_activity(const std::string& serial, const std::string& component);

private:
    ProcessResult run(const std::vector<std::string>& args, int timeout_ms = 10000);

    std::string path_;
    std::string version_;
};

// Candidate adb locations, in priority order. The user's own adb comes first:
// adb keeps a single server on port 5037, and starting a different version
// kills the running one, which would drop their Android Studio or scrcpy
// session out from under them.
std::vector<std::string> adb_search_paths();

} // namespace digitiz::host
