#include <doctest/doctest.h>

#include <digitiz/core/geometry.hpp>

using namespace digitiz::core;

TEST_CASE("pc <-> surface round-trips") {
    ViewTransform vt;
    vt.set(2.5, Vec2{-300.0, 17.5});

    for (const Vec2 pc : {Vec2{0, 0}, Vec2{1920, 1080}, Vec2{-1920, -120}, Vec2{7.25, -3.5}}) {
        const Vec2 back = vt.to_pc(vt.to_surface(pc));
        CHECK(back.x == doctest::Approx(pc.x));
        CHECK(back.y == doctest::Approx(pc.y));
    }
}

TEST_CASE("zoom_about pins the PC point under the anchor") {
    ViewTransform vt;
    vt.set(1.0, Vec2{100.0, 50.0});

    const Vec2 anchor{640.0, 360.0};
    const Vec2 before = vt.to_pc(anchor);

    vt.zoom_about(anchor, 2.0);
    const Vec2 after = vt.to_pc(anchor);

    CHECK(after.x == doctest::Approx(before.x));
    CHECK(after.y == doctest::Approx(before.y));
    CHECK(vt.scale() == doctest::Approx(2.0));
}

TEST_CASE("zoom_about still pins the anchor when the scale clamps") {
    ViewTransform vt;
    vt.set(ViewTransform::kMaxScale, Vec2{0.0, 0.0});

    const Vec2 anchor{200.0, 100.0};
    const Vec2 before = vt.to_pc(anchor);

    vt.zoom_about(anchor, 1000.0); // way past the ceiling
    const Vec2 after = vt.to_pc(anchor);

    CHECK(vt.scale() == doctest::Approx(ViewTransform::kMaxScale));
    CHECK(after.x == doctest::Approx(before.x));
    CHECK(after.y == doctest::Approx(before.y));
}

TEST_CASE("pan_by moves content with the finger") {
    ViewTransform vt;
    vt.set(2.0, Vec2{0.0, 0.0});

    const Vec2 surface_before = vt.to_surface(Vec2{100.0, 100.0});
    vt.pan_by(Vec2{40.0, -20.0});
    const Vec2 surface_after = vt.to_surface(Vec2{100.0, 100.0});

    CHECK(surface_after.x - surface_before.x == doctest::Approx(40.0));
    CHECK(surface_after.y - surface_before.y == doctest::Approx(-20.0));
}

TEST_CASE("fit frames the rect and centers it") {
    ViewTransform vt;
    vt.fit(Recti{.x = 0, .y = 0, .w = 1000, .h = 500}, 500.0, 500.0);

    CHECK(vt.scale() == doctest::Approx(0.5)); // width is the binding constraint

    const Vec2 tl = vt.to_surface(Vec2{0.0, 0.0});
    const Vec2 br = vt.to_surface(Vec2{1000.0, 500.0});

    CHECK(tl.x == doctest::Approx(0.0));
    CHECK(br.x == doctest::Approx(500.0));

    // Vertically centered: equal margins above and below.
    CHECK(tl.y == doctest::Approx(500.0 - br.y));
}

TEST_CASE("fit handles a negative virtual-desktop origin") {
    ViewTransform vt;
    vt.fit(Recti{.x = -1920, .y = -120, .w = 3840, .h = 1200}, 1080.0, 2400.0);

    const Vec2 c = vt.to_surface(Recti{-1920, -120, 3840, 1200}.center());
    CHECK(c.x == doctest::Approx(540.0));
    CHECK(c.y == doctest::Approx(1200.0));
}

TEST_CASE("fit with a margin still centers on the full viewport") {
    // The bug this guards: shrinking the viewport to make room for a margin
    // also shifts the centre, parking the content toward the top-left.
    ViewTransform vt;
    vt.fit(Recti{.x = 0, .y = 0, .w = 1920, .h = 1080}, 720.0, 1544.0, 0.12);

    const Vec2 c = vt.to_surface(Recti{0, 0, 1920, 1080}.center());
    CHECK(c.x == doctest::Approx(360.0));
    CHECK(c.y == doctest::Approx(772.0));

    // And the margin is real: the rect must not touch the viewport edges.
    const Vec2 tl = vt.to_surface(Vec2{0.0, 0.0});
    CHECK(tl.x > 0.0);
    CHECK(tl.x == doctest::Approx(720.0 * 0.06));
}

TEST_CASE("fit ignores degenerate input") {
    ViewTransform vt;
    vt.set(3.0, Vec2{1.0, 2.0});

    vt.fit(Recti{.x = 0, .y = 0, .w = 0, .h = 100}, 500.0, 500.0);

    CHECK(vt.scale() == doctest::Approx(3.0));
    CHECK(vt.pan().x == doctest::Approx(1.0));
}

TEST_CASE("grid_step_pc snaps to the 1/2/5 ladder") {
    CHECK(grid_step_pc(1.0, 80.0) == doctest::Approx(100.0));
    CHECK(grid_step_pc(0.5, 80.0) == doctest::Approx(200.0));
    CHECK(grid_step_pc(8.0, 80.0) == doctest::Approx(10.0));
    CHECK(grid_step_pc(80.0, 80.0) == doctest::Approx(1.0));
}

TEST_CASE("grid_step_pc keeps on-screen spacing within a sane band") {
    for (double scale = ViewTransform::kMinScale; scale <= ViewTransform::kMaxScale;
         scale *= 1.07) {
        const double surface_px = grid_step_pc(scale, 80.0) * scale;
        CHECK(surface_px >= 40.0);
        CHECK(surface_px <= 160.0);
    }
}

TEST_CASE("grid_step_pc survives degenerate scale") {
    CHECK(grid_step_pc(0.0, 80.0) == doctest::Approx(1.0));
    CHECK(grid_step_pc(-1.0, 80.0) == doctest::Approx(1.0));
}
