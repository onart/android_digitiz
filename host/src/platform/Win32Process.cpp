#ifdef _WIN32

#include "platform/Process.hpp"

#include <chrono>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace digitiz::host {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                        nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                          nullptr);
    return out;
}

// CommandLineToArgvW quoting: wrap in quotes, double any backslashes that
// immediately precede a quote, and escape embedded quotes.
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring out;
    out.push_back(L'"');
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

struct HandleGuard {
    HANDLE h = nullptr;
    ~HandleGuard() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
        }
    }
};

} // namespace

std::string find_executable(const std::string& name) {
    const std::wstring wname = widen(name);

    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD n = ::SearchPathW(nullptr, wname.c_str(), L".exe", static_cast<DWORD>(std::size(buffer)),
                                  buffer, nullptr);
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    return narrow(std::wstring(buffer, n));
}

ProcessResult run_process(const std::string& exe, const std::vector<std::string>& args,
                          int timeout_ms) {
    ProcessResult result;

    std::wstring exe_w = widen(exe);
    if (exe.find('\\') == std::string::npos && exe.find('/') == std::string::npos) {
        const std::string resolved = find_executable(exe);
        if (resolved.empty()) {
            return result; // launched stays false
        }
        exe_w = widen(resolved);
    }

    std::wstring cmdline = quote_arg(exe_w);
    for (const std::string& a : args) {
        cmdline.push_back(L' ');
        cmdline.append(quote_arg(widen(a)));
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HandleGuard read_end;
    HANDLE write_raw = nullptr;
    if (!::CreatePipe(&read_end.h, &write_raw, &sa, 0)) {
        return result;
    }
    HandleGuard write_end{write_raw};

    // Only the child should inherit the write end.
    ::SetHandleInformation(read_end.h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_end.h;
    si.hStdError = write_end.h;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    // CREATE_NO_WINDOW keeps adb from flashing a console window.
    const BOOL created =
        ::CreateProcessW(exe_w.c_str(), cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &si, &pi);
    if (!created) {
        return result;
    }

    HandleGuard proc{pi.hProcess};
    HandleGuard thread{pi.hThread};

    // The parent must drop its write end or the pipe never reports EOF.
    ::CloseHandle(write_end.h);
    write_end.h = nullptr;

    result.launched = true;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    char buffer[4096];
    for (;;) {
        DWORD available = 0;
        if (!::PeekNamedPipe(read_end.h, nullptr, 0, nullptr, &available, nullptr)) {
            break; // pipe closed
        }

        if (available > 0) {
            DWORD got = 0;
            const DWORD want = available < sizeof(buffer) ? available : sizeof(buffer);
            if (!::ReadFile(read_end.h, buffer, want, &got, nullptr) || got == 0) {
                break;
            }
            result.output.append(buffer, got);
            continue;
        }

        if (::WaitForSingleObject(proc.h, 0) == WAIT_OBJECT_0) {
            // Exited; one more peek catches anything written just before exit.
            DWORD leftover = 0;
            ::PeekNamedPipe(read_end.h, nullptr, 0, nullptr, &leftover, nullptr);
            if (leftover == 0) {
                break;
            }
            continue;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            ::TerminateProcess(proc.h, 1);
            result.timed_out = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    ::WaitForSingleObject(proc.h, 1000);

    DWORD code = 0;
    if (::GetExitCodeProcess(proc.h, &code)) {
        result.exit_code = static_cast<int>(code);
    }
    return result;
}

} // namespace digitiz::host

#endif // _WIN32
