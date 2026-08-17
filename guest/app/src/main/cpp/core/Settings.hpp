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

    const std::string& path() const noexcept { return path_; }

private:
    void save() const;

    std::string path_;
    bool auto_launch_ = true;
};

} // namespace digitiz::guest
