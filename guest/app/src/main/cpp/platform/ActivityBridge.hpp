#pragma once

// The things the guest needs from its Java half that are not text.
//
// TextRenderer keeps its own JNI attachment because it needs one on every
// rasterize; this is the other direction — occasional calls, made from
// wherever, that do not justify holding state.

#include <string>
#include <vector>

struct GameActivity;

namespace digitiz::guest {

// Turns the display the other way round and remembers the choice, so the next
// launch comes up the same way. Done through the activity rather than by
// rotating what we draw: that way the system bars, the display cutout and the
// touch mapping all move with it.
//
// Safe to call from the render thread — the activity marshals to the UI
// thread itself.
void flip_orientation(GameActivity* activity);

// --- custom button dialogs -------------------------------------------------
//
// Creating a button means typing numbers or a shortcut, and there is no text
// field in a GL surface. Both moments hand off to a real Android dialog and
// come back through the two drains below. Fields are passed as primitives so
// this file stays independent of the button store.

// `index` below zero opens the editor on a new button; the remaining
// arguments then only supply the defaults it starts on.
void show_button_editor(GameActivity* activity, int index, int kind, const std::string& label,
                        int x, int y, int w, int h, int modifiers, const std::string& key);

void show_button_menu(GameActivity* activity, int index, const std::string& label);

// Opens the preset menu. `names` is what to list, `current` which one is in
// use, and `active_window` the program the bind entry would attach to.
void show_preset_menu(GameActivity* activity, const std::vector<std::string>& names, int current,
                      const std::string& active_window);

// Must match MainActivity's PRESET_* constants.
enum class PresetCommandKind : int {
    Select = 0,
    Create = 1,
    Rename = 2,
    Bind = 3,
    Unbind = 4,
    Delete = 5,
};

struct PresetCommand {
    PresetCommandKind kind = PresetCommandKind::Select;
    int index = -1;
    std::string text;
};

struct ButtonEdit {
    int index = -1; // below zero creates
    int kind = 0;
    std::string label;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int modifiers = 0;
    std::string key;
};

// Must match MainActivity's COMMAND_* constants.
enum class ButtonCommandKind : int {
    Edit = -1, // reopen the editor on this button
    Delete = 0,
    Earlier = 1,
    Later = 2,
};

struct ButtonCommand {
    int index = -1;
    ButtonCommandKind kind = ButtonCommandKind::Delete;
};

// Everything the dialogs produced since the last call. They land on the UI
// thread; this hands them to the render thread, which owns the store.
void drain_button_events(std::vector<ButtonEdit>& edits, std::vector<ButtonCommand>& commands);

void drain_preset_events(std::vector<PresetCommand>& commands);

} // namespace digitiz::guest
