#pragma once

// The tiling arithmetic moved to common/ once the guest needed to run it too:
// see digitiz/proto/tiling.hpp for what it does and why it lives there.
//
// This header stays so host code can keep calling it unqualified.

#include <digitiz/proto/tiling.hpp>

namespace digitiz::host {

using proto::ViewportGeometry;
using proto::make_geometry;

} // namespace digitiz::host
