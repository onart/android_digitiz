#pragma once

namespace digitiz::host {

// Compresses one captured frame's blocks in both orders and reports what the
// difference would be worth. `divisor` shrinks the encoded surface the way the
// guest's resolution ratio does.
bool run_block_layout_probe(int divisor);

} // namespace digitiz::host
