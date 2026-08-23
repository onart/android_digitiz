#pragma once

// Guest-side settings, stored as a small key=value file.
//
// It lives in the app's external files directory on purpose. The host needs to
// know whether it may launch this app *while the app is not running*, so the
// app cannot answer over the link — but `adb shell cat` can read this file
// without the app being alive. That keeps the phone as the single source of
// truth instead of the host caching a copy that can go stale.
//
// GameActivity hands the directory to native code, so none of this needs JNI.

#include <string>

namespace digitiz::guest {

// Must match kGuestSettingsPath on the host.
inline constexpr const char* kSettingsFileName = "settings.txt";

class Settings {
public:
    // `external_dir` comes from GameActivity's externalDataPath. A null or
    // empty path leaves the settings in memory only.
    void load(const char* external_dir);

    bool auto_launch() const noexcept { return auto_launch_; }
    void set_auto_launch(bool on);

    // Pointer decimation while drawing. A sample is sent only once it is both
    // far enough and late enough, which thins a stroke into evenly spaced
    // points. Note this only decimates — the smoothing itself is the host's
    // spline; the two are meant to work together.
    int min_interval_ms() const noexcept { return min_interval_ms_; }
    float min_distance_dp() const noexcept { return min_distance_dp_; }
    void set_throttle(int interval_ms, float distance_dp);

    // How the custom button strip is arranged, and whether it is open. Both
    // are the user's arrangement of their own screen, so they outlive a run.
    bool strip_vertical() const noexcept { return strip_vertical_; }
    bool strip_expanded() const noexcept { return strip_expanded_; }
    void set_strip(bool vertical, bool expanded);

    // Buttons shown at once. Zero means the strip picks its own default.
    int strip_slots() const noexcept { return strip_slots_; }
    void set_strip_slots(int slots);

    const std::string& path() const noexcept { return path_; }

private:
    void save() const;

    std::string path_;
    bool auto_launch_ = true;
    int min_interval_ms_ = 0;
    float min_distance_dp_ = 0.0f;
    bool strip_vertical_ = false;
    bool strip_expanded_ = false;
    int strip_slots_ = 0;
};

} // namespace digitiz::guest
