#pragma once

// Runs a child process and captures its output. Used only to drive `adb`.

#include <string>
#include <vector>

namespace digitiz::host {

struct ProcessResult {
    bool launched = false; // false means the executable could not be started
    bool timed_out = false;
    int exit_code = -1;
    std::string output; // stdout and stderr, merged

    bool ok() const noexcept { return launched && !timed_out && exit_code == 0; }
};

// `exe` may be a bare name, in which case PATH is searched.
ProcessResult run_process(const std::string& exe, const std::vector<std::string>& args,
                          int timeout_ms = 10000);

// Resolves a bare executable name against PATH. Empty if not found.
std::string find_executable(const std::string& name);

} // namespace digitiz::host
