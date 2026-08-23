#include "buttons/ButtonStore.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

// Well past anything a person would make by hand. A file that says otherwise
// is corrupt, and refusing to grow without bound is cheaper than finding out
// what a million buttons does to the frame time.
constexpr std::size_t kMaxButtons = 256;
constexpr std::size_t kMaxPresets = 64;

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

int to_int(const std::string& s) {
    return std::atoi(s.c_str());
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lhs = static_cast<unsigned char>(a[i]);
        const auto rhs = static_cast<unsigned char>(b[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

// Fields 1..8 of a B record, which are also fields 0..7 of the pre-preset
// format. Returns false when there are not enough of them.
bool parse_button(const std::vector<std::string>& f, std::size_t at, CustomButton& out) {
    if (f.size() < at + 8) {
        return false;
    }
    const int kind = to_int(f[at]);
    if (kind < 0 || kind > static_cast<int>(ButtonKind::Shortcut)) {
        return false;
    }
    out.kind = static_cast<ButtonKind>(kind);
    out.label = sanitize_label(f[at + 1]);
    out.target = core::Recti{to_int(f[at + 2]), to_int(f[at + 3]), to_int(f[at + 4]),
                             to_int(f[at + 5])};
    out.modifiers = static_cast<std::uint8_t>(to_int(f[at + 6]));
    out.key = f[at + 7];
    return true;
}

} // namespace

std::string sanitize_label(std::string label) {
    std::erase_if(label, [](char c) { return c == '\t' || c == '\n' || c == '\r'; });
    if (label.size() > 24) {
        label.resize(24);
    }
    return label;
}

void ButtonStore::load(const char* external_dir) {
    if (external_dir == nullptr || *external_dir == '\0') {
        DZ_WARN("no external data directory; custom buttons will not persist");
        return;
    }
    path_ = std::string(external_dir) + "/" + kButtonsFileName;

    std::ifstream in(path_);
    if (!in) {
        DZ_INFO("no custom buttons yet (%s)", path_.c_str());
        return;
    }

    std::vector<Preset> loaded;
    std::size_t buttons = 0;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const std::vector<std::string> f = split_tabs(line);

        if (f[0] == "P") {
            if (loaded.size() >= kMaxPresets) {
                DZ_WARN("button file has more than %zu presets; ignoring the rest", kMaxPresets);
                break;
            }
            Preset preset;
            preset.name = f.size() > 1 ? sanitize_label(f[1]) : std::string();
            preset.match = f.size() > 2 ? sanitize_label(f[2]) : std::string();
            loaded.push_back(std::move(preset));
            continue;
        }

        // "B" records, and bare numbers from before presets existed.
        const std::size_t at = f[0] == "B" ? 1 : 0;
        CustomButton button;
        if (!parse_button(f, at, button)) {
            DZ_WARN("skipping malformed button line: %s", line.c_str());
            continue;
        }
        if (loaded.empty()) {
            loaded.push_back(Preset{});
        }
        if (buttons >= kMaxButtons) {
            DZ_WARN("button file has more than %zu buttons; ignoring the rest", kMaxButtons);
            break;
        }
        loaded.back().buttons.push_back(std::move(button));
        ++buttons;
    }

    if (!loaded.empty()) {
        presets_ = std::move(loaded);
    }
    current_ = 0;

    DZ_INFO("loaded %zu button(s) across %zu preset(s) from %s", buttons, presets_.size(),
            path_.c_str());
}

// --- the preset in use -----------------------------------------------------

void ButtonStore::add(CustomButton button) {
    if (mutable_buttons().size() >= kMaxButtons) {
        DZ_WARN("refusing to add a button: already at the %zu limit", kMaxButtons);
        return;
    }
    button.label = sanitize_label(std::move(button.label));
    mutable_buttons().push_back(std::move(button));
    save();
}

void ButtonStore::replace(int index, CustomButton button) {
    if (!valid(index)) {
        return;
    }
    button.label = sanitize_label(std::move(button.label));
    mutable_buttons()[static_cast<std::size_t>(index)] = std::move(button);
    save();
}

void ButtonStore::remove(int index) {
    if (!valid(index)) {
        return;
    }
    mutable_buttons().erase(mutable_buttons().begin() + index);
    save();
}

void ButtonStore::move(int index, int delta) {
    if (!valid(index) || delta == 0) {
        return;
    }
    const int last = static_cast<int>(mutable_buttons().size()) - 1;
    const int to = std::clamp(index + delta, 0, last);
    if (to == index) {
        return;
    }

    CustomButton moved = std::move(mutable_buttons()[static_cast<std::size_t>(index)]);
    mutable_buttons().erase(mutable_buttons().begin() + index);
    mutable_buttons().insert(mutable_buttons().begin() + to, std::move(moved));
    save();
}

// --- the presets themselves ------------------------------------------------

bool ButtonStore::select(int index) {
    if (!valid_preset(index) || index == current_) {
        return false;
    }
    current_ = index;
    return true;
}

void ButtonStore::create(std::string name) {
    if (presets_.size() >= kMaxPresets) {
        DZ_WARN("refusing to add a preset: already at the %zu limit", kMaxPresets);
        return;
    }
    Preset preset;
    preset.name = sanitize_label(std::move(name));
    presets_.push_back(std::move(preset));
    // Switching to it is the only sensible next step: it is empty, and the
    // reason to make one is to put something in it.
    current_ = static_cast<int>(presets_.size()) - 1;
    save();
}

void ButtonStore::rename(int index, std::string name) {
    if (!valid_preset(index)) {
        return;
    }
    presets_[static_cast<std::size_t>(index)].name = sanitize_label(std::move(name));
    save();
}

void ButtonStore::set_match(int index, std::string process) {
    if (!valid_preset(index)) {
        return;
    }
    presets_[static_cast<std::size_t>(index)].match = sanitize_label(std::move(process));
    save();
}

void ButtonStore::remove_preset(int index) {
    if (!valid_preset(index) || presets_.size() <= 1) {
        return;
    }
    presets_.erase(presets_.begin() + index);
    current_ = std::clamp(current_, 0, static_cast<int>(presets_.size()) - 1);
    save();
}

int ButtonStore::preset_for(const std::string& process) const {
    if (process.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < presets_.size(); ++i) {
        if (!presets_[i].match.empty() && iequals(presets_[i].match, process)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ButtonStore::save() const {
    if (path_.empty()) {
        return;
    }

    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
        DZ_WARN("could not write custom buttons to %s", path_.c_str());
        return;
    }
    out << "# digitiz buttons.  P\tname\tmatch   |   B\tkind\tlabel\tx\ty\tw\th\tmodifiers\tkey\n";
    for (const Preset& preset : presets_) {
        out << "P\t" << preset.name << '\t' << preset.match << '\n';
        for (const CustomButton& b : preset.buttons) {
            out << "B\t" << static_cast<int>(b.kind) << '\t' << b.label << '\t' << b.target.x
                << '\t' << b.target.y << '\t' << b.target.w << '\t' << b.target.h << '\t'
                << static_cast<int>(b.modifiers) << '\t' << b.key << '\n';
        }
    }
}

} // namespace digitiz::guest
