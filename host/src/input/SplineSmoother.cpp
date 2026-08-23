#include "input/SplineSmoother.hpp"

#include <algorithm>
#include <cmath>

namespace digitiz::host {

namespace {

constexpr int kMaxSubdivisions = 64;

double distance(double ax, double ay, double bx, double by) {
    const double dx = bx - ax;
    const double dy = by - ay;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

void SplineSmoother::begin(double x, double y) {
    // The first point is duplicated to stand in for the missing P0, which is
    // what gives the opening segment a defined tangent.
    points_[0] = Point{x, y};
    points_[1] = Point{x, y};
    count_ = 2;
}

void SplineSmoother::push(Point p) {
    if (count_ < points_.size()) {
        points_[count_++] = p;
        return;
    }
    points_[0] = points_[1];
    points_[1] = points_[2];
    points_[2] = points_[3];
    points_[3] = p;
}

void SplineSmoother::add(double x, double y, const Emit& emit) {
    if (!enabled_) {
        emit(x, y);
        return;
    }
    if (count_ == 0) {
        // A sample with no stroke open: nothing to interpolate against.
        begin(x, y);
        emit(x, y);
        return;
    }

    push(Point{x, y});
    if (count_ == points_.size()) {
        emit_segment(emit);
    }
}

void SplineSmoother::finish(const Emit& emit) {
    if (!enabled_ || count_ == 0) {
        reset();
        return;
    }

    // Pad with the last sample so the path ends exactly where the finger did.
    // A short stroke may not have filled the window yet, in which case each
    // push completes a segment that has never been drawn; a full window has
    // already drawn everything except the final sample's own segment.
    while (count_ < points_.size()) {
        push(points_[count_ - 1]);
        if (count_ == points_.size()) {
            emit_segment(emit);
        }
    }

    push(points_[3]);
    emit_segment(emit);

    reset();
}

void SplineSmoother::emit_segment(const Emit& emit) const {
    const Point& p0 = points_[0];
    const Point& p1 = points_[1];
    const Point& p2 = points_[2];
    const Point& p3 = points_[3];

    const double span = distance(p1.x, p1.y, p2.x, p2.y);
    if (span <= 1e-9) {
        return; // the finger did not move
    }

    // Knot spacing. With alpha 0.5 this is the square root of the chord
    // length, which is what keeps the curve inside its control polygon.
    const double t0 = 0.0;
    const double t1 = t0 + std::pow(distance(p0.x, p0.y, p1.x, p1.y), alpha_);
    const double t2 = t1 + std::pow(span, alpha_);
    const double t3 = t2 + std::pow(distance(p2.x, p2.y, p3.x, p3.y), alpha_);

    const int steps = std::clamp(static_cast<int>(std::ceil(span / step_px_)), 1,
                                 kMaxSubdivisions);

    // Coincident samples collapse a knot interval to zero and the barycentric
    // weights below divide by it. A straight line through the segment is the
    // right answer there anyway.
    if (t1 - t0 <= 1e-9 || t2 - t1 <= 1e-9 || t3 - t2 <= 1e-9) {
        for (int i = 1; i <= steps; ++i) {
            const double s = static_cast<double>(i) / steps;
            emit(p1.x + (p2.x - p1.x) * s, p1.y + (p2.y - p1.y) * s);
        }
        return;
    }

    for (int i = 1; i <= steps; ++i) {
        const double t = t1 + (t2 - t1) * (static_cast<double>(i) / steps);

        const double a1x = (t1 - t) / (t1 - t0) * p0.x + (t - t0) / (t1 - t0) * p1.x;
        const double a1y = (t1 - t) / (t1 - t0) * p0.y + (t - t0) / (t1 - t0) * p1.y;
        const double a2x = (t2 - t) / (t2 - t1) * p1.x + (t - t1) / (t2 - t1) * p2.x;
        const double a2y = (t2 - t) / (t2 - t1) * p1.y + (t - t1) / (t2 - t1) * p2.y;
        const double a3x = (t3 - t) / (t3 - t2) * p2.x + (t - t2) / (t3 - t2) * p3.x;
        const double a3y = (t3 - t) / (t3 - t2) * p2.y + (t - t2) / (t3 - t2) * p3.y;

        const double b1x = (t2 - t) / (t2 - t0) * a1x + (t - t0) / (t2 - t0) * a2x;
        const double b1y = (t2 - t) / (t2 - t0) * a1y + (t - t0) / (t2 - t0) * a2y;
        const double b2x = (t3 - t) / (t3 - t1) * a2x + (t - t1) / (t3 - t1) * a3x;
        const double b2y = (t3 - t) / (t3 - t1) * a2y + (t - t1) / (t3 - t1) * a3y;

        emit((t2 - t) / (t2 - t1) * b1x + (t - t1) / (t2 - t1) * b2x,
             (t2 - t) / (t2 - t1) * b1y + (t - t1) / (t2 - t1) * b2y);
    }
}

} // namespace digitiz::host
