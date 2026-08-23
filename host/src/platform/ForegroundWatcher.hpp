#pragma once

// Which program the user is actually working in.
//
// The guest keys its button presets off this: a preset for Krita should come
// up when Krita has the focus and go away when it does not.

#include <cstdint>
#include <memory>
#include <string>

#include <digitiz/core/geometry.hpp>

namespace digitiz::host {

struct ForegroundWindow {
    std::uint32_t pid = 0;
    // Bare executable name, e.g. "krita.exe". Empty when the window could not
    // be identified — a process can refuse to be opened even for a name.
    std::string process;

    bool operator==(const ForegroundWindow&) const = default;
};

class IForegroundWatcher {
public:
    virtual ~IForegroundWatcher() = default;

    // Fills `out` and returns true only when the focused program has changed
    // since the last call, so the caller can treat every true as an event.
    //
    // A change is reported once it has held still for a moment. Alt-tabbing
    // from one program to another passes through the shell — the taskbar
    // really does take the focus for about a tenth of a second — and a preset
    // that dropped to its default and came back on every switch would be
    // worse than useless. Genuinely switching to the shell still reports it;
    // it just has to be the place the focus came to rest.
    //
    // Polled rather than hooked. SetWinEventHook(EVENT_SYSTEM_FOREGROUND) is
    // the obvious answer and would fire a few milliseconds sooner, but it only
    // works while the thread that installed it is pumping messages, needs a
    // file-static back pointer because the callback carries no user data, and
    // is known to stay quiet through some focus changes. Asking the OS once a
    // frame costs a pointer comparison — the expensive part, resolving the
    // name, only runs when the window actually changed — and cannot miss.
    virtual bool poll(ForegroundWindow& out) = 0;

    // The visible frame of whatever has the focus right now, in desktop
    // pixels. Read live rather than from the poll above, because a window can
    // be dragged without the focus ever changing.
    //
    // Called from the transport thread on every window-relative pointer, so it
    // must not touch the polling state.
    virtual bool focused_window_bounds(core::Recti& out) const = 0;
};

// Returns nullptr on platforms without an implementation yet.
std::unique_ptr<IForegroundWatcher> make_foreground_watcher();

} // namespace digitiz::host
