#pragma once

// The canonical coordinate space is PC virtual-desktop pixels. ViewTransform is
// the guest's only piece of view state: it maps that space onto the device
// surface and back.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace digitiz::core {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    friend Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend Vec2 operator*(Vec2 a, double s) noexcept { return {a.x * s, a.y * s}; }
    friend Vec2 operator/(Vec2 a, double s) noexcept { return {a.x / s, a.y / s}; }
};

struct Recti {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;

    Vec2 center() const noexcept { return {x + w / 2.0, y + h / 2.0}; }

    bool contains(std::int32_t px, std::int32_t py) const noexcept {
        return px >= x && py >= y && px < x + w && py < y + h;
    }

    bool operator==(const Recti&) const noexcept = default;
};

// surface = (pc - pan) * scale
// pc      = surface / scale + pan
class ViewTransform {
public:
    static constexpr double kMinScale = 0.02;
    static constexpr double kMaxScale = 20.0;

    double scale() const noexcept { return scale_; }
    Vec2 pan() const noexcept { return pan_; }

    Vec2 to_surface(Vec2 pc) const noexcept { return (pc - pan_) * scale_; }
    Vec2 to_pc(Vec2 surface) const noexcept { return surface / scale_ + pan_; }

    // Scales about a surface-space anchor, keeping the PC point under that
    // anchor pinned. This is what makes a pinch feel attached to the fingers.
    void zoom_about(Vec2 surface_anchor, double factor) noexcept {
        const Vec2 pc_anchor = to_pc(surface_anchor);
        scale_ = std::clamp(scale_ * factor, kMinScale, kMaxScale);
        pan_ = pc_anchor - surface_anchor / scale_;
    }

    // Drags the content by a surface-space delta.
    void pan_by(Vec2 surface_delta) noexcept { pan_ = pan_ - surface_delta / scale_; }

    // Frames `rect` inside a surface_w x surface_h viewport, centered.
    //
    // `margin` is the fraction of the viewport to leave empty around the rect,
    // so 0.12 keeps a 6% gap on each side. Note the scale comes from the inset
    // area but the centering uses the *full* viewport — shrinking both is the
    // easy mistake, and it parks the content off toward one corner.
    void fit(Recti rect, double surface_w, double surface_h, double margin = 0.0) noexcept {
        if (rect.w <= 0 || rect.h <= 0 || surface_w <= 0.0 || surface_h <= 0.0) {
            return;
        }
        const double usable = std::clamp(1.0 - margin, 0.05, 1.0);
        const double s = std::min(surface_w * usable / rect.w, surface_h * usable / rect.h);
        scale_ = std::clamp(s, kMinScale, kMaxScale);
        pan_ = rect.center() - Vec2{surface_w / 2.0, surface_h / 2.0} / scale_;
    }

    void set(double scale, Vec2 pan) noexcept {
        scale_ = std::clamp(scale, kMinScale, kMaxScale);
        pan_ = pan;
    }

private:
    double scale_ = 1.0;
    Vec2 pan_{};
};

// Grid spacing in PC pixels, chosen so lines land roughly `target_surface_px`
// apart on screen. Snaps to a 1/2/5 x 10^n ladder so zooming steps cleanly
// instead of drifting through ugly intervals.
inline double grid_step_pc(double scale, double target_surface_px = 80.0) noexcept {
    if (scale <= 0.0 || target_surface_px <= 0.0) {
        return 1.0;
    }
    const double raw = target_surface_px / scale;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag; // [1, 10)
    double mult = 10.0;
    if (norm < 1.5) {
        mult = 1.0;
    } else if (norm < 3.5) {
        mult = 2.0;
    } else if (norm < 7.5) {
        mult = 5.0;
    }
    return mult * mag;
}

} // namespace digitiz::core
