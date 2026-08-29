#pragma once

// Host-side settings, stored as a small key=value file beside the executable.
//
// The guest's settings file exists so the PC can read it while the app is not
// running. This one exists for a plainer reason: everything the user set was
// lost on every restart, which during development is every rebuild. Injection
// in particular had to be switched back on by hand each time, and that is
// exactly the kind of friction that ends with someone chasing a feature that
// was simply turned off.
//
// Same shape as the guest's file on purpose -- one key=value per line, written
// whole -- so either end reads the same way.

#include <string>

namespace digitiz::host {

inline constexpr const char* kHostSettingsFileName = "host.txt";

class HostSettings {
public:
    // The values the code would use if there were no file. Called before
    // load() so a key missing from the file leaves the default where it
    // already lives, instead of this class carrying a second copy of it.
    void seed(bool injection, int sweep_tiles, bool smoothing, double step_px,
              double alpha) noexcept;

    // An empty directory keeps everything in memory. That is what the headless
    // self-test wants: it must not write over what the user set.
    void load(const std::string& dir);

    // Whether pointers are injected. Remembered across runs, which means the
    // host can come up already injecting -- see the note where it is applied.
    bool injection() const noexcept { return injection_; }
    void set_injection(bool on);

    int sweep_tiles() const noexcept { return sweep_tiles_; }
    void set_sweep_tiles(int tiles);

    bool smoothing() const noexcept { return smoothing_; }
    double smoothing_step_px() const noexcept { return smoothing_step_px_; }
    double smoothing_alpha() const noexcept { return smoothing_alpha_; }
    void set_smoothing(bool on, double step_px, double alpha);

    const std::string& path() const noexcept { return path_; }

private:
    void save() const;

    std::string path_;
    bool injection_ = false;
    int sweep_tiles_ = 1;
    bool smoothing_ = false;
    double smoothing_step_px_ = 2.0;
    double smoothing_alpha_ = 0.5;
};

} // namespace digitiz::host
