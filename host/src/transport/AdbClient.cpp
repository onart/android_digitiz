#include "transport/AdbClient.hpp"

#include <cstdlib>
#include <sstream>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

namespace {

std::string env_var(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

std::string join(const std::string& dir, const char* tail) {
    if (dir.empty()) {
        return {};
    }
#ifdef _WIN32
    return dir + "\\" + tail;
#else
    return dir + "/" + tail;
#endif
}

#ifdef _WIN32
constexpr const char* kAdbName = "adb.exe";
#else
constexpr const char* kAdbName = "adb";
#endif

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

} // namespace

std::vector<std::string> adb_search_paths() {
    std::vector<std::string> out;

    // 1. Whatever is on PATH. Reusing the user's adb avoids restarting their
    //    server with a different version.
    const std::string on_path = find_executable("adb");
    if (!on_path.empty()) {
        out.push_back(on_path);
    }

    // 2. The usual SDK locations.
    for (const char* var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        const std::string root = env_var(var);
        if (!root.empty()) {
            out.push_back(join(join(root, "platform-tools"), kAdbName));
        }
    }

#ifdef _WIN32
    const std::string local = env_var("LOCALAPPDATA");
    if (!local.empty()) {
        out.push_back(join(join(join(local, "Android"), "Sdk"), std::string("platform-tools\\" + std::string(kAdbName)).c_str()));
    }
#else
    const std::string home = env_var("HOME");
    if (!home.empty()) {
        out.push_back(home + "/Android/Sdk/platform-tools/adb");
        out.push_back(home + "/Library/Android/sdk/platform-tools/adb");
    }
#endif

    return out;
}

bool AdbClient::init() {
    if (ready()) {
        return true;
    }

    for (const std::string& candidate : adb_search_paths()) {
        if (candidate.empty()) {
            continue;
        }
        const ProcessResult r = run_process(candidate, {"version"}, 8000);
        if (!r.ok()) {
            continue;
        }

        path_ = candidate;
        std::istringstream stream(r.output);
        std::getline(stream, version_);
        version_ = trim(version_);

        DZ_INFO("adb: %s", path_.c_str());
        DZ_INFO("adb: %s", version_.c_str());

        // Starting the server here surfaces a version clash in the log rather
        // than at the first device query.
        const ProcessResult start = run({"start-server"}, 15000);
        if (!start.ok()) {
            DZ_WARN("adb start-server returned %d: %s", start.exit_code,
                    trim(start.output).c_str());
        }
        return true;
    }

    DZ_ERROR("adb not found. Put it on PATH or set ANDROID_HOME.");
    return false;
}

ProcessResult AdbClient::run(const std::vector<std::string>& args, int timeout_ms) {
    if (!ready()) {
        return {};
    }
    return run_process(path_, args, timeout_ms);
}

std::vector<AdbDevice> AdbClient::devices() {
    std::vector<AdbDevice> out;

    const ProcessResult r = run({"devices", "-l"}, 8000);
    if (!r.ok()) {
        return out;
    }

    std::istringstream stream(r.output);
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (first) {
            first = false; // "List of devices attached"
            continue;
        }
        if (line.empty()) {
            continue;
        }

        std::istringstream fields(line);
        AdbDevice device;
        if (!(fields >> device.serial >> device.state)) {
            continue;
        }

        std::string token;
        while (fields >> token) {
            constexpr std::string_view kModel = "model:";
            if (token.rfind(kModel, 0) == 0) {
                device.model = token.substr(kModel.size());
            }
        }

        out.push_back(std::move(device));
    }

    return out;
}

bool AdbClient::reverse(const std::string& serial, int device_port, int host_port) {
    const ProcessResult r = run({"-s", serial, "reverse", "tcp:" + std::to_string(device_port),
                                 "tcp:" + std::to_string(host_port)},
                                8000);
    if (!r.ok()) {
        DZ_ERROR("adb reverse failed (%d): %s", r.exit_code, trim(r.output).c_str());
        return false;
    }
    DZ_INFO("adb reverse: device tcp:%d -> host tcp:%d", device_port, host_port);
    return true;
}

bool AdbClient::reverse_remove(const std::string& serial, int device_port) {
    const ProcessResult r =
        run({"-s", serial, "reverse", "--remove", "tcp:" + std::to_string(device_port)}, 8000);
    if (!r.ok()) {
        // Not worth an error: the device is usually just gone already.
        DZ_DEBUG("adb reverse --remove returned %d", r.exit_code);
        return false;
    }
    return true;
}

bool AdbClient::is_app_running(const std::string& serial, const std::string& package) {
    // pidof prints nothing and exits non-zero when the package is not running,
    // so the output is the reliable signal rather than the exit code.
    const ProcessResult r = run({"-s", serial, "shell", "pidof", package}, 6000);
    return !trim(r.output).empty();
}

bool AdbClient::start_activity(const std::string& serial, const std::string& component) {
    const ProcessResult r = run({"-s", serial, "shell", "am", "start", "-n", component}, 10000);

    // `am start` exits 0 even when it prints an error, so check the text too.
    const std::string output = trim(r.output);
    if (!r.ok() || output.find("Error") != std::string::npos) {
        DZ_WARN("could not launch guest (%s): %s", component.c_str(), output.c_str());
        return false;
    }
    DZ_INFO("launched guest: %s", component.c_str());
    return true;
}

} // namespace digitiz::host
