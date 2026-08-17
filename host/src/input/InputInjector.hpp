#pragma once

// Turns virtual-desktop coordinates into real OS mouse input.
//
// Windows is the only implementation for milestone 1. Linux (/dev/uinput) and
// macOS (CGEvent) slot in behind this same interface later.

#include <cstdint>
#include <memory>

#include <digitiz/core/geometry.hpp>
#include <digitiz/proto/messages.hpp>

namespace digitiz::host {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;

    // Absolute coordinates are normalized against this rect, so it must track
    // the real virtual desktop or every injected point lands wrong.
    virtual void set_virtual_bounds(core::Recti bounds) = 0;
    virtual core::Recti virtual_bounds() const = 0;

    virtual bool move_to(std::int32_t x, std::int32_t y) = 0;

    // Move and press/release are issued as one event so a physical mouse
    // cannot slip between them and drop the click somewhere else.
    virtual bool button(proto::MouseButton b, bool down, std::int32_t x, std::int32_t y) = 0;

    // Releases anything still held. Must be called when a session ends —
    // otherwise the user is left with a stuck mouse button.
    virtual void release_all() = 0;

    virtual bool any_button_down() const = 0;

    // Reads back the OS cursor position. Used by the coordinate self-test.
    virtual bool cursor_pos(std::int32_t& x, std::int32_t& y) const = 0;

    // Coordinates clamped because they fell outside the virtual desktop.
    virtual std::uint64_t clamped_count() const = 0;
};

// Returns nullptr on platforms without an implementation yet.
std::unique_ptr<IInputInjector> make_input_injector();

} // namespace digitiz::host
