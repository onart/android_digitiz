#include "core/Settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

constexpr const char* kAutoLaunchKey = "auto_launch";
constexpr const char* kMinIntervalKey = "min_interval_ms";
constexpr const char* kMinDistanceKey = "min_distance_dp";
constexpr const char* kStripVerticalKey = "strip_vertical";
constexpr const char* kStripExpandedKey = "strip_expanded";

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
        } else if (key == kMinIntervalKey) {
            min_interval_ms_ = std::atoi(value.c_str());
        } else if (key == kMinDistanceKey) {
            min_distance_dp_ = static_cast<float>(std::atof(value.c_str()));
        } else if (key == kStripVerticalKey) {
            strip_vertical_ = value != "0";
        } else if (key == kStripExpandedKey) {
            strip_expanded_ = value != "0";
        }
    }

    DZ_INFO("settings loaded from %s (auto_launch=%d, min_interval=%d ms, min_distance=%.1f dp)",
            path_.c_str(), auto_launch_ ? 1 : 0, min_interval_ms_,
            static_cast<double>(min_distance_dp_));
}

void Settings::set_throttle(int interval_ms, float distance_dp) {
    if (min_interval_ms_ == interval_ms && min_distance_dp_ == distance_dp) {
        return;
    }
    min_interval_ms_ = interval_ms;
    min_distance_dp_ = distance_dp;
    save();
}

void Settings::set_strip(bool vertical, bool expanded) {
    if (strip_vertical_ == vertical && strip_expanded_ == expanded) {
        return;
    }
    strip_vertical_ = vertical;
    strip_expanded_ = expanded;
    save();
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

    // Written whole every time; the file is a handful of lines and the host
    // may read it at any moment, so a partial rewrite is not worth the risk.
    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
        DZ_WARN("could not write settings to %s", path_.c_str());
        return;
    }
    out << "# digitiz guest settings\n";
    out << kAutoLaunchKey << "=" << (auto_launch_ ? 1 : 0) << "\n";
    out << kMinIntervalKey << "=" << min_interval_ms_ << "\n";
    out << kMinDistanceKey << "=" << min_distance_dp_ << "\n";
    out << kStripVerticalKey << "=" << (strip_vertical_ ? 1 : 0) << "\n";
    out << kStripExpandedKey << "=" << (strip_expanded_ ? 1 : 0) << "\n";
}

} // namespace digitiz::guest
