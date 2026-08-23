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
    // Tap clicks one place on the PC.
    Point = 0,
    // The button is a scale model of a PC rectangle: touching it clicks the
    // matching spot inside that rectangle, dragging drags there. The view does
    // not move.
    Region = 1,
    // Tap sends a key combination.
    Shortcut = 2,
};

struct CustomButton {
    ButtonKind kind = ButtonKind::Point;
    std::string label;
    // Point uses x/y. Region uses all four. Shortcut uses none.
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
