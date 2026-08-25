#pragma once

// Runs the capture on its own for a few seconds and says what it saw.
//
// The tile scheduler is built on assumptions about what a desktop actually
// reports — that most frames touch a small part of the screen, and that a
// scroll comes back as a move rather than as a full repaint. Those are claims
// about someone else's driver, so they get measured rather than believed.
//
// Needs no phone and no guest: run the host with --capture-test, use the PC
// normally for a few seconds, and read the summary.

#include <string>

namespace digitiz::host {

struct CaptureProbeResult {
    int frames = 0;
    int mouse_only = 0;
    int full_frames = 0;
    int frames_with_moves = 0;
    int dirty_rects = 0;
    int move_rects = 0;
    std::uint32_t coalesced = 0; // frames the driver merged because we were slow
    double dirty_fraction_mean = 0.0;
    double dirty_fraction_max = 0.0;
    double interval_ms_mean = 0.0;
};

// Runs for `seconds`, then reports. Writes the last captured frame to
// `bmp_path` if it is not empty, so the pixels can be looked at.
bool run_capture_probe(int seconds, const std::string& bmp_path, CaptureProbeResult& out);

} // namespace digitiz::host
