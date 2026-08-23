#ifdef _WIN32

#include "input/InputInjector.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "input/AbsoluteCoord.hpp"
#include "input/KeyNames.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

namespace {

constexpr std::size_t kButtonCount = 3;

std::size_t button_index(proto::MouseButton b) noexcept {
    switch (b) {
    case proto::MouseButton::Left:
        return 0;
    case proto::MouseButton::Right:
        return 1;
    case proto::MouseButton::Middle:
        return 2;
    }
    return 0;
}

DWORD button_flag(proto::MouseButton b, bool down) noexcept {
    switch (b) {
    case proto::MouseButton::Left:
        return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    case proto::MouseButton::Right:
        return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    case proto::MouseButton::Middle:
        return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }
    return 0;
}

// Modifier bit -> virtual key, in the order they are pressed. Released in
// reverse, which is what an application watching the key state expects.
struct ModifierKey {
    std::uint8_t bit;
    std::uint16_t vk;
};

constexpr std::array kModifierKeys{
    ModifierKey{proto::kModCtrl, VK_CONTROL},
    ModifierKey{proto::kModShift, VK_SHIFT},
    ModifierKey{proto::kModAlt, VK_MENU},
    ModifierKey{proto::kModMeta, VK_LWIN},
};

// Extended keys need the flag or Windows delivers the numpad twin instead.
bool is_extended(std::uint16_t vk) noexcept {
    switch (vk) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_DIVIDE:
    case VK_NUMLOCK:
    case VK_SNAPSHOT:
    case VK_RCONTROL:
    case VK_RMENU:
        return true;
    default:
        return false;
    }
}

INPUT key_input(std::uint16_t vk, bool down) noexcept {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = (down ? 0u : KEYEVENTF_KEYUP) | (is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
    return in;
}

class Win32InputInjector final : public IInputInjector {
public:
    void set_virtual_bounds(core::Recti bounds) override {
        if (bounds.w > 0 && bounds.h > 0) {
            bounds_ = bounds;
        }
    }

    core::Recti virtual_bounds() const override { return bounds_; }

    bool move_to(std::int32_t x, std::int32_t y) override { return emit(x, y, 0); }

    bool button(proto::MouseButton b, bool down, std::int32_t x, std::int32_t y) override {
        if (!emit(x, y, button_flag(b, down))) {
            return false;
        }
        held_[button_index(b)] = down;
        return true;
    }

    bool key(const proto::Key& k) override {
        std::uint16_t vk = 0;
        if (!key_name_to_code(k.key, vk)) {
            DZ_WARN("injector: unknown key name \"%s\"", k.key.c_str());
            return false;
        }

        // Built as one batch and sent in a single call: SendInput delivers an
        // array atomically with respect to other threads' input, so nothing of
        // the user's can land between the modifier and the key.
        std::vector<INPUT> batch;
        batch.reserve(kModifierKeys.size() * 2 + 2);

        const bool press_phase = k.action != proto::KeyAction::Up;
        const bool release_phase = k.action != proto::KeyAction::Down;

        if (press_phase) {
            for (const ModifierKey& m : kModifierKeys) {
                if ((k.modifiers & m.bit) != 0) {
                    batch.push_back(key_input(m.vk, true));
                }
            }
            batch.push_back(key_input(vk, true));
        }
        if (release_phase) {
            batch.push_back(key_input(vk, false));
            for (auto it = kModifierKeys.rbegin(); it != kModifierKeys.rend(); ++it) {
                if ((k.modifiers & it->bit) != 0) {
                    batch.push_back(key_input(it->vk, false));
                }
            }
        }

        if (::SendInput(static_cast<UINT>(batch.size()), batch.data(), sizeof(INPUT)) !=
            batch.size()) {
            DZ_ERROR("injector: SendInput failed for key \"%s\" (GetLastError=%lu)",
                     k.key.c_str(), ::GetLastError());
            return false;
        }

        // Only a bare Down leaves anything behind, and that is the one case
        // release_all() has to be able to undo.
        if (!release_phase) {
            held_keys_.push_back(vk);
            held_mods_ |= k.modifiers;
        } else {
            std::erase(held_keys_, vk);
            if (k.action == proto::KeyAction::Up) {
                held_mods_ &= static_cast<std::uint8_t>(~k.modifiers);
            }
        }
        return true;
    }

    void release_all() override {
        for (const std::uint16_t vk : held_keys_) {
            INPUT in = key_input(vk, false);
            ::SendInput(1, &in, sizeof(INPUT));
            DZ_WARN("injector: force-released stuck key 0x%02X", vk);
        }
        held_keys_.clear();

        for (auto it = kModifierKeys.rbegin(); it != kModifierKeys.rend(); ++it) {
            if ((held_mods_ & it->bit) == 0) {
                continue;
            }
            INPUT in = key_input(it->vk, false);
            ::SendInput(1, &in, sizeof(INPUT));
            DZ_WARN("injector: force-released stuck modifier 0x%02X", it->vk);
        }
        held_mods_ = 0;

        for (std::size_t i = 0; i < kButtonCount; ++i) {
            if (!held_[i]) {
                continue;
            }
            const auto b = static_cast<proto::MouseButton>(i);
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = button_flag(b, false); // no move: release in place
            ::SendInput(1, &in, sizeof(INPUT));
            held_[i] = false;
            DZ_WARN("injector: force-released stuck %s button", proto::to_string(b));
        }
    }

    bool any_button_down() const override {
        return std::any_of(held_.begin(), held_.end(), [](bool v) { return v; });
    }

    bool cursor_pos(std::int32_t& x, std::int32_t& y) const override {
        POINT p{};
        if (!::GetCursorPos(&p)) {
            return false;
        }
        x = p.x;
        y = p.y;
        return true;
    }

    std::uint64_t clamped_count() const override { return clamped_; }

private:
    // See AbsoluteCoord.hpp for why the conversion is a ceiling division.
    void normalize(std::int32_t x, std::int32_t y, LONG& nx, LONG& ny) {
        const std::int64_t w = std::max<std::int32_t>(bounds_.w, 1);
        const std::int64_t h = std::max<std::int32_t>(bounds_.h, 1);

        const std::int64_t rx = static_cast<std::int64_t>(x) - bounds_.x;
        const std::int64_t ry = static_cast<std::int64_t>(y) - bounds_.y;

        const std::int64_t cx = std::clamp<std::int64_t>(rx, 0, w - 1);
        const std::int64_t cy = std::clamp<std::int64_t>(ry, 0, h - 1);
        if (cx != rx || cy != ry) {
            ++clamped_;
        }

        nx = static_cast<LONG>(pixel_to_normalized(cx, w));
        ny = static_cast<LONG>(pixel_to_normalized(cy, h));
    }

    bool emit(std::int32_t x, std::int32_t y, DWORD extra_flags) {
        LONG nx = 0;
        LONG ny = 0;
        normalize(x, y, nx, ny);

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = nx;
        in.mi.dy = ny;
        in.mi.dwFlags =
            MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | extra_flags;

        if (::SendInput(1, &in, sizeof(INPUT)) == 1) {
            return true;
        }

        // The usual cause is UIPI: a more-privileged window has focus and this
        // process is not elevated.
        DZ_ERROR("injector: SendInput failed (GetLastError=%lu). An elevated window may have "
                 "focus; run the host as administrator to inject into it.",
                 ::GetLastError());
        return false;
    }

    core::Recti bounds_{0, 0, 1, 1};
    std::array<bool, kButtonCount> held_{};
    std::vector<std::uint16_t> held_keys_;
    std::uint8_t held_mods_ = 0;
    std::uint64_t clamped_ = 0;
};

} // namespace

std::unique_ptr<IInputInjector> make_input_injector() {
    return std::make_unique<Win32InputInjector>();
}

} // namespace digitiz::host

#endif // _WIN32
