#pragma once

namespace digitiz::host {

// Runs the compute encoder against the CPU one on a handful of images and
// reports the difference. Builds its own device: no capture, no phone.
bool run_etc2_compute_test();

} // namespace digitiz::host
