#include "app/HostSettings.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

namespace {

constexpr const char* kInjectionKey = "injection";
constexpr const char* kSweepTilesKey = "sweep_tiles";
constexpr const char* kSmoothingKey = "smoothing";
constexpr const char* kSmoothingStepKey = "smoothing_step_px";
constexpr const char* kSmoothingAlphaKey = "smoothing_alpha";

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

void HostSettings::seed(bool injection, int sweep_tiles, bool smoothing, double step_px,
                        double alpha) noexcept {
    injection_ = injection;
    sweep_tiles_ = sweep_tiles;
    smoothing_ = smoothing;
    smoothing_step_px_ = step_px;
    smoothing_alpha_ = alpha;
}

void HostSettings::load(const std::string& dir) {
    if (dir.empty()) {
        return;
    }
    path_ = dir + "/" + kHostSettingsFileName;

    std::ifstream in(path_);
    if (!in) {
        // First run. Write the seeded defaults out now, so the file is
        // something to edit rather than something to guess at.
        DZ_INFO("no host settings file yet, writing defaults to %s", path_.c_str());
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

        if (key == kInjectionKey) {
            injection_ = value != "0";
        } else if (key == kSweepTilesKey) {
            sweep_tiles_ = std::atoi(value.c_str());
        } else if (key == kSmoothingKey) {
            smoothing_ = value != "0";
        } else if (key == kSmoothingStepKey) {
            smoothing_step_px_ = std::atof(value.c_str());
        } else if (key == kSmoothingAlphaKey) {
            smoothing_alpha_ = std::atof(value.c_str());
        }
    }

    DZ_INFO("host settings loaded from %s (injection=%d, sweep_tiles=%d)", path_.c_str(),
            injection_ ? 1 : 0, sweep_tiles_);
}

void HostSettings::set_injection(bool on) {
    if (injection_ == on) {
        return;
    }
    injection_ = on;
    save();
}

void HostSettings::set_sweep_tiles(int tiles) {
    if (sweep_tiles_ == tiles) {
        return;
    }
    sweep_tiles_ = tiles;
    save();
}

void HostSettings::set_smoothing(bool on, double step_px, double alpha) {
    if (smoothing_ == on && smoothing_step_px_ == step_px && smoothing_alpha_ == alpha) {
        return;
    }
    smoothing_ = on;
    smoothing_step_px_ = step_px;
    smoothing_alpha_ = alpha;
    save();
}

void HostSettings::save() const {
    if (path_.empty()) {
        return;
    }

    // Written whole every time; it is a handful of lines, and a partial
    // rewrite is not worth the risk for something this small.
    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
        DZ_WARN("could not write host settings to %s", path_.c_str());
        return;
    }
    out << "# digitiz host settings\n";
    out << kInjectionKey << "=" << (injection_ ? 1 : 0) << "\n";
    out << kSweepTilesKey << "=" << sweep_tiles_ << "\n";
    out << kSmoothingKey << "=" << (smoothing_ ? 1 : 0) << "\n";
    out << kSmoothingStepKey << "=" << smoothing_step_px_ << "\n";
    out << kSmoothingAlphaKey << "=" << smoothing_alpha_ << "\n";
}

} // namespace digitiz::host
