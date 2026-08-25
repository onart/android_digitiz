#pragma once

#include <string>

namespace digitiz::host {

// Runs the send path for `seconds` with no phone attached, reassembles the
// batches back into an image, and writes it out. `divisor` scales the encoded
// surface down from the desktop.
//
// The pieces each have their own tests; this is the one that can tell whether
// they fit together, because tiles landing in the wrong places produces a
// picture that is wrong in a way only an eye catches.
bool run_frame_probe(int seconds, int divisor, const std::string& bmp_path);

} // namespace digitiz::host
