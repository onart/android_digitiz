#pragma once

// The user's custom buttons, and the file they live in.
//
// Stored beside settings.txt in the app's external files directory, one button
// per line, tab separated. Not JSON: the fields are flat and fixed, a parser
// would be more code than the whole file, and `adb shell cat` on a line format
// is readable when something goes wrong. Unknown trailing fields are ignored
// on load, so a later version can add one without orphaning existing buttons.

#include <cstdint>
#include <string>
#include <vector>

#include <digitiz/core/geometry.hpp>

namespace digitiz::guest {

inline constexpr const char* kButtonsFileName = "buttons.txt";

enum class ButtonKind : std::uint8_t {
    // Tap clicks one place, given as an offset inside the focused window.
    Point = 0,
    // A scale model of a rectangle: touching it clicks the matching spot
    // inside. Retired -- it reads well on paper and is fiddly in the hand, a
    // postage stamp standing in for a toolbar. The editor no longer offers it
    // and dragging one now scrolls the strip instead, but buttons already on a
    // device keep working, and the number stays reserved either way.
    Region = 1,
    // Tap sends a key combination.
    Shortcut = 2,
};

struct CustomButton {
    ButtonKind kind = ButtonKind::Point;
    std::string label;
    // Point uses x/y, Region all four, Shortcut none.
    //
    // Offsets inside the host's focused window, not desktop pixels. A button
    // that means "the save icon" has to keep meaning that when the window is
    // moved or opened on another monitor.
    core::Recti target{};
    std::uint8_t modifiers = 0; // proto::kMod* bits
    std::string key;            // key name, lowercase

    bool operator==(const CustomButton&) const = default;
};

class ButtonStore {
public:
    void load(const char* external_dir);

    const std::vector<CustomButton>& buttons() const noexcept { return buttons_; }
    std::size_t size() const noexcept { return buttons_.size(); }
    bool valid(int index) const noexcept {
        return index >= 0 && static_cast<std::size_t>(index) < buttons_.size();
    }

    void add(CustomButton button);
    void replace(int index, CustomButton button);
    void remove(int index);
    // Slides one button along the list by `delta`, clamped to the ends. The
    // list is displayed in order, so this is how the user arranges it.
    void move(int index, int delta);

private:
    void save() const;

    std::string path_;
    std::vector<CustomButton> buttons_;
};

// Anything that would break the line format is stripped rather than escaped:
// a tab in a button label is never what someone meant to type.
std::string sanitize_label(std::string label);

} // namespace digitiz::guest
