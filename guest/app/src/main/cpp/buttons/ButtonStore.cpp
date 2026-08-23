#include "buttons/ButtonStore.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

// Well past anything a person would make by hand. A file that says otherwise
// is corrupt, and refusing to grow without bound is cheaper than finding out
// what a million buttons does to the frame time.
constexpr std::size_t kMaxButtons = 256;

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

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const std::vector<std::string> f = split_tabs(line);
        if (f.size() < 8) {
            DZ_WARN("skipping malformed button line: %s", line.c_str());
            continue;
        }

        const int kind = to_int(f[0]);
        if (kind < 0 || kind > static_cast<int>(ButtonKind::Shortcut)) {
            DZ_WARN("skipping button with unknown kind %d", kind);
            continue;
        }

        CustomButton b;
        b.kind = static_cast<ButtonKind>(kind);
        b.label = sanitize_label(f[1]);
        b.target = core::Recti{to_int(f[2]), to_int(f[3]), to_int(f[4]), to_int(f[5])};
        b.modifiers = static_cast<std::uint8_t>(to_int(f[6]));
        b.key = f[7];
        buttons_.push_back(std::move(b));

        if (buttons_.size() >= kMaxButtons) {
            DZ_WARN("button file has more than %zu entries; ignoring the rest", kMaxButtons);
            break;
        }
    }

    DZ_INFO("loaded %zu custom button(s) from %s", buttons_.size(), path_.c_str());
}

void ButtonStore::add(CustomButton button) {
    if (buttons_.size() >= kMaxButtons) {
        DZ_WARN("refusing to add a button: already at the %zu limit", kMaxButtons);
        return;
    }
    button.label = sanitize_label(std::move(button.label));
    buttons_.push_back(std::move(button));
    save();
}

void ButtonStore::replace(int index, CustomButton button) {
    if (!valid(index)) {
        return;
    }
    button.label = sanitize_label(std::move(button.label));
    buttons_[static_cast<std::size_t>(index)] = std::move(button);
    save();
}

void ButtonStore::remove(int index) {
    if (!valid(index)) {
        return;
    }
    buttons_.erase(buttons_.begin() + index);
    save();
}

void ButtonStore::move(int index, int delta) {
    if (!valid(index) || delta == 0) {
        return;
    }
    const int last = static_cast<int>(buttons_.size()) - 1;
    const int to = std::clamp(index + delta, 0, last);
    if (to == index) {
        return;
    }

    CustomButton moved = std::move(buttons_[static_cast<std::size_t>(index)]);
    buttons_.erase(buttons_.begin() + index);
    buttons_.insert(buttons_.begin() + to, std::move(moved));
    save();
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
    out << "# digitiz custom buttons: kind\tlabel\tx\ty\tw\th\tmodifiers\tkey\n";
    for (const CustomButton& b : buttons_) {
        out << static_cast<int>(b.kind) << '\t' << b.label << '\t' << b.target.x << '\t'
            << b.target.y << '\t' << b.target.w << '\t' << b.target.h << '\t'
            << static_cast<int>(b.modifiers) << '\t' << b.key << '\n';
    }
}

} // namespace digitiz::guest
