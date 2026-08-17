#pragma once

// Shows where the current view sits inside the PC desktop.
//
// Only useful while zoomed in far enough that the desktop outline runs off the
// screen — at that point there is nothing on screen saying which part of the
// PC the finger is over. Once milestone 2 streams the desktop image, the image
// itself answers that, so this hides.
//
// Deliberately does NOT consume touch. On a drawing surface a region that
// silently swallows strokes is worse than one that is slightly obscured.

#include <span>

#include <digitiz/core/geometry.hpp>

#include "render/UiRenderer.hpp"

namespace digitiz::guest {

class Minimap {
public:
    // Milestone 2 turns this off once the real desktop image is being shown.
    void set_enabled(bool on) noexcept { enabled_ = on; }
    bool enabled() const noexcept { return enabled_; }

    bool visible(const core::ViewTransform& view, int surface_w, int surface_h,
                 core::Recti desktop) const;

    // `desktop` is the bounding box the map is drawn against; `monitors` are
    // the real screens laid out inside it, so a gap between them is visible
    // here too rather than reading as one continuous surface.
    void draw(UiRenderer& ui, const core::ViewTransform& view, int surface_w, int surface_h,
              core::Recti desktop, std::span<const core::Recti> monitors, float density) const;

private:
    bool enabled_ = true;
};

} // namespace digitiz::guest
