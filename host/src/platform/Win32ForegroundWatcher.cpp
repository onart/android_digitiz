#ifdef _WIN32

#include "platform/ForegroundWatcher.hpp"

#include <chrono>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace digitiz::host {

namespace {

// Long enough to sit out the shell taking the focus mid-alt-tab, short enough
// that a real switch feels immediate. Measured on this machine: the taskbar
// holds it for roughly 100 ms.
constexpr auto kSettleDelay = std::chrono::milliseconds(200);

std::string narrow(const wchar_t* s, int len) {
    if (len <= 0) {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    return out;
}

// The file name, not the path: a preset matches on "krita.exe" and should not
// care where it was installed.
std::string process_name_of(DWORD pid) {
    if (pid == 0) {
        return {};
    }

    // QUERY_LIMITED_INFORMATION exists precisely for this: it can name a
    // process that a full QUERY_INFORMATION open would be refused for.
    // Elevated and protected processes can still refuse, and that is reported
    // as an unidentified window rather than guessed at.
    const HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return {};
    }

    wchar_t path[MAX_PATH] = {};
    DWORD len = static_cast<DWORD>(std::size(path));
    const BOOL ok = ::QueryFullProcessImageNameW(proc, 0, path, &len);
    ::CloseHandle(proc);

    if (!ok || len == 0) {
        return {};
    }

    const wchar_t* begin = path;
    for (DWORD i = 0; i < len; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') {
            begin = path + i + 1;
        }
    }
    return narrow(begin, static_cast<int>(len - (begin - path)));
}

class Win32ForegroundWatcher final : public IForegroundWatcher {
public:
    bool poll(ForegroundWindow& out) override {
        const HWND hwnd = ::GetForegroundWindow();

        // Nothing has the focus at all. Momentary, and treating it as a state
        // to report would mean an empty name in the middle of every switch.
        if (hwnd == nullptr) {
            return false;
        }

        // The cheap check first: the same window is the overwhelmingly common
        // answer, and resolving a process name is the only part with a cost.
        if (hwnd != last_hwnd_) {
            last_hwnd_ = hwnd;

            DWORD pid = 0;
            ::GetWindowThreadProcessId(hwnd, &pid);

            ForegroundWindow fresh;
            fresh.pid = pid;
            fresh.process = process_name_of(pid);

            // Compared on the answer, not the window: moving between two
            // windows of one program is not a change of program, and a preset
            // keyed on the program has no reason to hear about it. This also
            // means passing back through a program mid-switch cancels the
            // pending change rather than queueing a second one.
            // `seen_` guards the very first poll: a window nobody can name
            // compares equal to the empty starting value, and without this it
            // would never be reported at all.
            if (seen_ && fresh == pending_) {
                return false;
            }
            pending_ = std::move(fresh);
            pending_since_ = std::chrono::steady_clock::now();
            settling_ = pending_ != current_ || !seen_;
        }

        if (!settling_ || std::chrono::steady_clock::now() - pending_since_ < kSettleDelay) {
            return false;
        }

        settling_ = false;
        seen_ = true;
        current_ = pending_;
        out = current_;
        return true;
    }

private:
    HWND last_hwnd_ = nullptr;
    // What the focus is on now, versus what has been reported. They differ
    // only while a change is settling.
    ForegroundWindow pending_;
    ForegroundWindow current_;
    std::chrono::steady_clock::time_point pending_since_{};
    bool settling_ = false;
    bool seen_ = false;
};

} // namespace

std::unique_ptr<IForegroundWatcher> make_foreground_watcher() {
    return std::make_unique<Win32ForegroundWatcher>();
}

} // namespace digitiz::host

#endif // _WIN32
