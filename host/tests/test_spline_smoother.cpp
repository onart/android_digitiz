#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "input/SplineSmoother.hpp"

using namespace digitiz::host;

namespace {

struct Path {
    std::vector<std::pair<double, double>> points;

    SplineSmoother::Emit sink() {
        return [this](double x, double y) { points.emplace_back(x, y); };
    }

    bool ends_at(double x, double y) const {
        return !points.empty() && std::abs(points.back().first - x) < 1e-6 &&
               std::abs(points.back().second - y) < 1e-6;
    }
};

SplineSmoother make(double step = 2.0) {
    SplineSmoother s;
    s.set_enabled(true);
    s.set_step_px(step);
    return s;
}

} // namespace

TEST_CASE("disabled, every sample passes straight through") {
    SplineSmoother s; // enabled defaults to false
    Path path;

    s.begin(0, 0);
    s.add(10, 0, path.sink());
    s.add(20, 0, path.sink());
    s.finish(path.sink());

    REQUIRE(path.points.size() == 2);
    CHECK(path.points[0].first == doctest::Approx(10.0));
    CHECK(path.points[1].first == doctest::Approx(20.0));
}

TEST_CASE("nothing is drawn until the next sample defines the curve") {
    SplineSmoother s = make();
    Path path;

    s.begin(0, 0);
    s.add(10, 0, path.sink());
    // One sample in, the segment to it is still undefined: Catmull-Rom needs
    // the point after it. This lag is the whole cost of the feature.
    CHECK(path.points.empty());

    s.add(20, 0, path.sink());
    CHECK_FALSE(path.points.empty());
    // ...and what it drew is the first segment, not the second.
    CHECK(path.points.back().first == doctest::Approx(10.0));
}

TEST_CASE("the path ends exactly on the last sample") {
    SplineSmoother s = make();
    Path path;

    s.begin(0, 0);
    s.add(10, 5, path.sink());
    s.add(20, 0, path.sink());
    s.add(30, 8, path.sink());
    s.finish(path.sink());

    CHECK(path.ends_at(30, 8));
}

TEST_CASE("a short stroke that never filled the window still completes") {
    SplineSmoother s = make();
    Path path;

    s.begin(0, 0);
    s.add(10, 0, path.sink());
    s.finish(path.sink());

    CHECK(path.ends_at(10, 0));
}

TEST_CASE("a stroke with no movement emits nothing") {
    SplineSmoother s = make();
    Path path;

    s.begin(5, 5);
    s.finish(path.sink());

    CHECK(path.points.empty());
}

TEST_CASE("a straight line stays straight") {
    SplineSmoother s = make();
    Path path;

    s.begin(0, 0);
    for (int x = 10; x <= 100; x += 10) {
        s.add(x, 0, path.sink());
    }
    s.finish(path.sink());

    REQUIRE(path.points.size() > 10);
    for (const auto& [x, y] : path.points) {
        CHECK(std::abs(y) < 1e-6); // no overshoot off the line
    }
}

TEST_CASE("the path advances monotonically along a straight line") {
    SplineSmoother s = make();
    Path path;

    s.begin(0, 0);
    for (int x = 10; x <= 60; x += 10) {
        s.add(x, 0, path.sink());
    }
    s.finish(path.sink());

    double previous = -1.0;
    for (const auto& [x, y] : path.points) {
        CHECK(x >= previous - 1e-9);
        previous = x;
    }
}

TEST_CASE("a corner is rounded but stays near the control points") {
    SplineSmoother s = make();
    Path path;

    // An L: right along y=0, then up at x=100.
    s.begin(0, 0);
    s.add(50, 0, path.sink());
    s.add(100, 0, path.sink());
    s.add(100, 50, path.sink());
    s.add(100, 100, path.sink());
    s.finish(path.sink());

    REQUIRE_FALSE(path.points.empty());
    // Centripetal Catmull-Rom keeps the curve inside a sane envelope; uniform
    // is what overshoots here.
    for (const auto& [x, y] : path.points) {
        CHECK(x >= -5.0);
        CHECK(x <= 115.0);
        CHECK(y >= -15.0);
        CHECK(y <= 115.0);
    }
    CHECK(path.ends_at(100, 100));
}

TEST_CASE("repeated identical samples do not divide by zero") {
    SplineSmoother s = make();
    Path path;

    s.begin(10, 10);
    s.add(10, 10, path.sink());
    s.add(10, 10, path.sink());
    s.add(20, 10, path.sink());
    s.add(20, 10, path.sink());
    s.finish(path.sink());

    for (const auto& [x, y] : path.points) {
        CHECK(std::isfinite(x));
        CHECK(std::isfinite(y));
    }
    CHECK(path.ends_at(20, 10));
}

TEST_CASE("smaller spacing produces more points over the same stroke") {
    const auto run = [](double step) {
        SplineSmoother s = make(step);
        Path path;
        s.begin(0, 0);
        s.add(40, 0, path.sink());
        s.add(80, 10, path.sink());
        s.add(120, 0, path.sink());
        s.finish(path.sink());
        return path.points.size();
    };

    CHECK(run(1.0) > run(8.0));
}

TEST_CASE("the subdivision count is capped so one huge jump cannot flood") {
    SplineSmoother s = make(0.25);
    Path path;

    s.begin(0, 0);
    s.add(100000, 0, path.sink());
    s.add(200000, 0, path.sink());
    s.finish(path.sink());

    // Three segments at most, each capped.
    CHECK(path.points.size() <= 3 * 64);
}

TEST_CASE("reset drops the stroke without emitting") {
    SplineSmoother s = make();
    Path path;

    s.begin(0, 0);
    s.add(10, 0, path.sink());
    s.add(20, 0, path.sink());
    const std::size_t before = path.points.size();

    s.reset();
    s.finish(path.sink());

    CHECK(path.points.size() == before);
    CHECK_FALSE(s.active());
}

TEST_CASE("a fresh stroke after reset does not join onto the old one") {
    SplineSmoother s = make();
    Path path;

    s.begin(0, 0);
    s.add(10, 0, path.sink());
    s.reset();

    s.begin(500, 500);
    s.add(510, 500, path.sink());
    s.finish(path.sink());

    for (const auto& [x, y] : path.points) {
        CHECK(x >= 499.0);
        CHECK(y >= 499.0);
    }
}
