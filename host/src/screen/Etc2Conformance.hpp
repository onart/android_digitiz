#pragma once

namespace digitiz::host {

// Encodes a probe image, has the graphics driver decode it, and compares that
// against our own decoder. Passing means the bit layout means the same thing
// to someone else's ETC2 implementation as it does to ours -- which the
// round-trip unit tests cannot tell us, since they only prove the encoder and
// the decoder here agree with each other.
//
// Run with --etc2-test. Needs a GL 4.3 context and no phone.
bool run_etc2_conformance();

} // namespace digitiz::host
