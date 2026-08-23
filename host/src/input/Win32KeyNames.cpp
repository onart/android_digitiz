#ifdef _WIN32

#include "input/KeyNames.hpp"

#include <array>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace digitiz::host {

namespace {

struct NamedKey {
    std::string_view name;
    std::uint16_t code;
};

// Only the keys a shortcut is plausibly built from. Anything missing is
// reported as unknown, which is a better answer than a near miss.
constexpr std::array kNamedKeys{
    NamedKey{"escape", VK_ESCAPE},   NamedKey{"esc", VK_ESCAPE},
    NamedKey{"tab", VK_TAB},         NamedKey{"enter", VK_RETURN},
    NamedKey{"return", VK_RETURN},   NamedKey{"space", VK_SPACE},
    NamedKey{"backspace", VK_BACK},  NamedKey{"delete", VK_DELETE},
    NamedKey{"del", VK_DELETE},      NamedKey{"insert", VK_INSERT},
    NamedKey{"home", VK_HOME},       NamedKey{"end", VK_END},
    NamedKey{"pageup", VK_PRIOR},    NamedKey{"pagedown", VK_NEXT},
    NamedKey{"up", VK_UP},           NamedKey{"down", VK_DOWN},
    NamedKey{"left", VK_LEFT},       NamedKey{"right", VK_RIGHT},
    NamedKey{"printscreen", VK_SNAPSHOT},
    NamedKey{"pause", VK_PAUSE},     NamedKey{"capslock", VK_CAPITAL},
    NamedKey{"numlock", VK_NUMLOCK}, NamedKey{"scrolllock", VK_SCROLL},
    NamedKey{"menu", VK_APPS},

    // OEM keys, named by the character they carry on a US layout. The codes
    // are layout-independent; the names are not, which is the usual trade and
    // the one every hotkey editor makes.
    NamedKey{"minus", VK_OEM_MINUS}, NamedKey{"equal", VK_OEM_PLUS},
    NamedKey{"comma", VK_OEM_COMMA}, NamedKey{"period", VK_OEM_PERIOD},
    NamedKey{"slash", VK_OEM_2},     NamedKey{"backslash", VK_OEM_5},
    NamedKey{"semicolon", VK_OEM_1}, NamedKey{"quote", VK_OEM_7},
    NamedKey{"leftbracket", VK_OEM_4}, NamedKey{"rightbracket", VK_OEM_6},
    NamedKey{"backquote", VK_OEM_3},

    NamedKey{"numpadplus", VK_ADD},        NamedKey{"numpadminus", VK_SUBTRACT},
    NamedKey{"numpadmultiply", VK_MULTIPLY}, NamedKey{"numpaddivide", VK_DIVIDE},
    NamedKey{"numpaddecimal", VK_DECIMAL},
};

bool parse_indexed(std::string_view name, std::string_view prefix, int first, int last,
                   std::uint16_t base, std::uint16_t& code) {
    if (!name.starts_with(prefix) || name.size() <= prefix.size()) {
        return false;
    }
    int value = 0;
    for (const char c : name.substr(prefix.size())) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > last) {
            return false;
        }
    }
    if (value < first) {
        return false;
    }
    code = static_cast<std::uint16_t>(base + (value - first));
    return true;
}

} // namespace

bool key_name_to_code(std::string_view name, std::uint16_t& code) {
    if (name.empty()) {
        return false;
    }

    // A single letter or digit is its own virtual key on Windows, which is why
    // these need no table.
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') {
            code = static_cast<std::uint16_t>('A' + (c - 'a'));
            return true;
        }
        if (c >= 'A' && c <= 'Z') {
            code = static_cast<std::uint16_t>(c);
            return true;
        }
        if (c >= '0' && c <= '9') {
            code = static_cast<std::uint16_t>(c);
            return true;
        }
        return false;
    }

    if (parse_indexed(name, "f", 1, 24, VK_F1, code)) {
        return true;
    }
    if (parse_indexed(name, "numpad", 0, 9, VK_NUMPAD0, code)) {
        return true;
    }

    for (const NamedKey& k : kNamedKeys) {
        if (k.name == name) {
            code = k.code;
            return true;
        }
    }
    return false;
}

} // namespace digitiz::host

#endif // _WIN32
