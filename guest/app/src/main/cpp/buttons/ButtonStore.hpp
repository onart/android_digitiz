#pragma once

// The user's custom buttons, grouped into presets, and the file they live in.
//
// Stored beside settings.txt in the app's external files directory, one record
// per line, tab separated. Not JSON: the fields are flat and fixed, a parser
// would be more code than the whole file, and `adb shell cat` on a line format
// is readable when something goes wrong.
//
//   P <name> <match>                     opens a preset
//   B <kind> <label> <x> <y> <w> <h> <mods> <key>    a button in it
//
// A line beginning with a digit is a button from before presets existed and is
// read into the first preset, so an existing device keeps its buttons.

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

struct Preset {
    // Empty on the preset that exists because one always has to; the UI shows
    // a localized name for it rather than inventing an English one down here.
    std::string name;
    // Executable name to offer this preset for, as ACTIVE_WINDOW reports it.
    // Empty means it is only ever chosen by hand.
    std::string match;
    std::vector<CustomButton> buttons;
};

class ButtonStore {
public:
    void load(const char* external_dir);

    // --- the preset in use ---

    const std::vector<CustomButton>& buttons() const noexcept {
        return presets_[static_cast<std::size_t>(current_)].buttons;
    }
    std::size_t size() const noexcept { return buttons().size(); }
    bool valid(int index) const noexcept {
        return index >= 0 && static_cast<std::size_t>(index) < buttons().size();
    }

    void add(CustomButton button);
    void replace(int index, CustomButton button);
    void remove(int index);
    // Slides one button along the list by `delta`, clamped to the ends. The
    // list is displayed in order, so this is how the user arranges it.
    void move(int index, int delta);

    // --- the presets themselves ---

    const std::vector<Preset>& presets() const noexcept { return presets_; }
    int current() const noexcept { return current_; }
    const Preset& current_preset() const noexcept {
        return presets_[static_cast<std::size_t>(current_)];
    }
    bool valid_preset(int index) const noexcept {
        return index >= 0 && static_cast<std::size_t>(index) < presets_.size();
    }

    // True when the selection actually moved.
    bool select(int index);
    void create(std::string name);
    void rename(int index, std::string name);
    void set_match(int index, std::string process);
    // The last preset cannot be removed: something has to hold the buttons.
    void remove_preset(int index);

    // Which preset asks for this program, or -1. Matched case-insensitively,
    // because nothing guarantees how a process name is capitalised.
    int preset_for(const std::string& process) const;

private:
    void save() const;
    std::vector<CustomButton>& mutable_buttons() {
        return presets_[static_cast<std::size_t>(current_)].buttons;
    }

    std::string path_;
    // Never empty: load() puts an unnamed one in if the file did not.
    std::vector<Preset> presets_{Preset{}};
    int current_ = 0;
};

// Anything that would break the line format is stripped rather than escaped:
// a tab in a button label is never what someone meant to type.
std::string sanitize_label(std::string label);

} // namespace digitiz::guest
