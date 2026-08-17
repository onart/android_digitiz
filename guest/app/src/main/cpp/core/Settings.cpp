#include "core/Settings.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

constexpr const char* kAutoLaunchKey = "auto_launch";

std::string trim(std::string s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front())) {
        s.erase(s.begin());
    }
    while (!s.empty() && space(s.back())) {
        s.pop_back();
    }
    return s;
}

} // namespace

void Settings::load(const char* external_dir) {
    if (external_dir == nullptr || *external_dir == '\0') {
        DZ_WARN("no external data directory; settings will not persist");
        return;
    }

    path_ = std::string(external_dir) + "/" + kSettingsFileName;

    std::ifstream in(path_);
    if (!in) {
        // First run. Write the defaults out now so the host has something to
        // read before the user has touched anything.
        DZ_INFO("no settings file yet, writing defaults to %s", path_.c_str());
        save();
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == kAutoLaunchKey) {
            auto_launch_ = value != "0";
        }
    }

    DZ_INFO("settings loaded from %s (auto_launch=%d)", path_.c_str(),
            auto_launch_ ? 1 : 0);
}

void Settings::set_auto_launch(bool on) {
    if (auto_launch_ == on) {
        return;
    }
    auto_launch_ = on;
    save();
    DZ_INFO("auto launch %s", on ? "enabled" : "disabled");
}

void Settings::save() const {
    if (path_.empty()) {
        return;
    }

    // Written whole every time; the file is two lines and the host may read it
    // at any moment, so a partial rewrite is not worth the risk.
    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
        DZ_WARN("could not write settings to %s", path_.c_str());
        return;
    }
    out << "# digitiz guest settings\n";
    out << kAutoLaunchKey << "=" << (auto_launch_ ? 1 : 0) << "\n";
}

} // namespace digitiz::guest
