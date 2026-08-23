#include "ui/Minimap.hpp"

#include <algorithm>

namespace digitiz::guest {

namespace {

// Top-left, because the side menu handle lives on the right edge.
constexpr float kMarginDp = 12.0f;
constexpr float kMaxWidthDp = 150.0f;
constexpr float kMaxHeightDp = 110.0f;

struct Layout {
    Rect frame;  // the whole desktop, in surface pixels
    Rect window; // the part currently on screen
};

Layout compute(const core::ViewTransform& view, int surface_w, int surface_h, core::Recti desktop,
               float density, float inset_x) {
    const float margin = kMarginDp * density;

    // Preserve the desktop's aspect inside the allowed box.
    const float aspect = static_cast<float>(desktop.h) / static_cast<float>(desktop.w);
    float w = kMaxWidthDp * density;
    float h = w * aspect;
    if (h > kMaxHeightDp * density) {
        h = kMaxHeightDp * density;
        w = h / aspect;
    }
    w = std::min(w, static_cast<float>(surface_w) * 0.4f);
    h = std::min(h, static_cast<float>(surface_h) * 0.4f);

    Layout out;
    out.frame = Rect{margin + inset_x, margin, w, h};

    // Where the viewport lands in desktop coordinates.
    const core::Vec2 tl = view.to_pc(core::Vec2{0.0, 0.0});
    const core::Vec2 br = view.to_pc(core::Vec2{static_cast<double>(surface_w),
                                                static_cast<double>(surface_h)});

    const auto to_frame_x = [&](double pc_x) {
        const double t = (pc_x - desktop.x) / static_cast<double>(desktop.w);
        return out.frame.x + static_cast<float>(std::clamp(t, 0.0, 1.0)) * out.frame.w;
    };
    const auto to_frame_y = [&](double pc_y) {
        const double t = (pc_y - desktop.y) / static_cast<double>(desktop.h);
        return out.frame.y + static_cast<float>(std::clamp(t, 0.0, 1.0)) * out.frame.h;
    };

    const float x0 = to_frame_x(tl.x);
    const float y0 = to_frame_y(tl.y);
    const float x1 = to_frame_x(br.x);
    const float y1 = to_frame_y(br.y);

    // Keep it visible even when the view is a sliver of the desktop.
    const float min_side = 3.0f * density;
    out.window = Rect{x0, y0, std::max(x1 - x0, min_side), std::max(y1 - y0, min_side)};
    return out;
}

} // namespace

bool Minimap::visible(const core::ViewTransform& view, int surface_w, int surface_h,
                      core::Recti desktop) const {
    if (!enabled_ || desktop.w <= 0 || desktop.h <= 0) {
        return false;
    }

    // Nothing to explain while the whole desktop outline is on screen.
    const core::Vec2 tl = view.to_surface(core::Vec2{static_cast<double>(desktop.x),
                                                     static_cast<double>(desktop.y)});
    const core::Vec2 br = view.to_surface(
        core::Vec2{static_cast<double>(desktop.x + desktop.w),
                   static_cast<double>(desktop.y + desktop.h)});

    const bool fully_visible = tl.x >= 0.0 && tl.y >= 0.0 &&
                               br.x <= static_cast<double>(surface_w) &&
                               br.y <= static_cast<double>(surface_h);
    return !fully_visible;
}

void Minimap::draw(UiRenderer& ui, const core::ViewTransform& view, int surface_w, int surface_h,
                   core::Recti desktop, std::span<const core::Recti> monitors, float density,
                   float inset_x) const {
    if (!visible(view, surface_w, surface_h, desktop)) {
        return;
    }

    const Layout layout = compute(view, surface_w, surface_h, desktop, density, inset_x);
    const float radius = 3.0f * density;

    // Backing plate, dark enough to read the outlines against the grid but
    // translucent so it does not feel like a hole in the canvas.
    ui.rounded_rect(Rect{layout.frame.x - 4.0f * density, layout.frame.y - 4.0f * density,
                         layout.frame.w + 8.0f * density, layout.frame.h + 8.0f * density},
                    radius + 2.0f * density, Color{0.05f, 0.055f, 0.07f, 0.72f});

    // Each screen, placed inside the bounding box. Drawing them separately
    // rather than filling the box keeps any dead corner between monitors
    // visible — that is exactly where a touch would do nothing.
    const auto place = [&](core::Recti r) {
        const float sx = layout.frame.w / static_cast<float>(desktop.w);
        const float sy = layout.frame.h / static_cast<float>(desktop.h);
        return Rect{layout.frame.x + static_cast<float>(r.x - desktop.x) * sx,
                    layout.frame.y + static_cast<float>(r.y - desktop.y) * sy,
                    static_cast<float>(r.w) * sx, static_cast<float>(r.h) * sy};
    };

    for (const core::Recti& m : monitors) {
        const Rect cell = place(m);
        ui.rounded_rect(cell, radius, Color{0.13f, 0.145f, 0.175f, 0.9f});
        ui.rounded_rect_outline(cell, radius, 1.5f * density,
                                Color{0.55f, 0.60f, 0.70f, 0.85f});
    }

    // The part of it currently on screen.
    ui.rounded_rect(layout.window, radius * 0.5f, Color{0.95f, 0.72f, 0.30f, 0.22f});
    ui.rounded_rect_outline(layout.window, radius * 0.5f, 1.5f * density,
                            Color{0.98f, 0.76f, 0.34f, 0.95f});
}

} // namespace digitiz::guest
