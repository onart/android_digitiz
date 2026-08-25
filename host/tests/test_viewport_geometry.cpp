#include <doctest/doctest.h>

#include "screen/ViewportGeometry.hpp"

using namespace digitiz::host;
using digitiz::core::Recti;

TEST_CASE("a request is rounded into something a block encoder can take") {
    // Both dimensions have to be whole 4x4 blocks, and rounding down would
    // drop the last few columns of the region as a visible strip of nothing.
    const ViewportGeometry g = make_geometry(Recti{0, 0, 1000, 700}, 333, 111, 60);
    CHECK(g.out_w % 4 == 0);
    CHECK(g.out_h % 4 == 0);
    CHECK(g.out_w >= 333);
    CHECK(g.out_h >= 111);
    CHECK(g.tile % 4 == 0);
    CHECK(g.valid());
}

TEST_CASE("asking for nothing asks for the region at its own size") {
    const ViewportGeometry g = make_geometry(Recti{10, 20, 640, 480}, 0, 0, 0);
    CHECK(g.out_w == 640);
    CHECK(g.out_h == 480);
    CHECK(g.tile == 64);
}

TEST_CASE("a region with no area cannot be served") {
    CHECK_FALSE(make_geometry(Recti{0, 0, 0, 100}, 0, 0, 0).valid());
    CHECK_FALSE(make_geometry(Recti{0, 0, 100, -5}, 0, 0, 0).valid());
}

TEST_CASE("the grid covers the surface, edges included") {
    // 200 is not a whole number of 64s, so the last column and row are short
    // and must still be part of the grid.
    const ViewportGeometry g = make_geometry(Recti{0, 0, 200, 200}, 200, 200, 64);
    CHECK(g.cols() == 4);
    CHECK(g.rows() == 4);
    CHECK(g.tile_count() == 16);

    const Recti last = g.tile_output_rect(15);
    CHECK(last.x == 192);
    CHECK(last.y == 192);
    CHECK(last.w == 8); // 200 - 192
    CHECK(last.h == 8);
    // Still whole blocks, or the encoder would refuse it.
    CHECK(last.w % 4 == 0);
    CHECK(last.h % 4 == 0);
}

TEST_CASE("tiles tile: no gap and no overlap across the surface") {
    const ViewportGeometry g = make_geometry(Recti{0, 0, 300, 140}, 300, 140, 64);
    long long covered = 0;
    for (int i = 0; i < g.tile_count(); ++i) {
        const Recti r = g.tile_output_rect(i);
        CHECK(r.w > 0);
        CHECK(r.h > 0);
        CHECK(r.x + r.w <= g.out_w);
        CHECK(r.y + r.h <= g.out_h);
        covered += static_cast<long long>(r.w) * r.h;
    }
    CHECK(covered == static_cast<long long>(g.out_w) * g.out_h);
}

TEST_CASE("source rectangles meet edge to edge, leaving no desktop column behind") {
    // Scaled by an awkward factor, which is where a rounding scheme that maps
    // origin-plus-size instead of edge-to-edge leaves seams.
    const ViewportGeometry g = make_geometry(Recti{100, 50, 1000, 500}, 320, 160, 64);
    for (int row = 0; row < g.rows(); ++row) {
        for (int col = 0; col + 1 < g.cols(); ++col) {
            const Recti a = g.tile_source_rect(row * g.cols() + col);
            const Recti b = g.tile_source_rect(row * g.cols() + col + 1);
            CHECK(a.x + a.w == b.x);
        }
    }
    const Recti first = g.tile_source_rect(0);
    CHECK(first.x == 100);
    CHECK(first.y == 50);

    const Recti last = g.tile_source_rect(g.tile_count() - 1);
    CHECK(last.x + last.w == 100 + 1000);
    CHECK(last.y + last.h == 50 + 500);
}

TEST_CASE("a dirty rectangle marks the tiles it touches, rounded outwards") {
    const ViewportGeometry g = make_geometry(Recti{0, 0, 256, 256}, 256, 256, 64);

    // Entirely inside tile (0,0).
    Recti tiles = g.tiles_touching(Recti{10, 10, 20, 20});
    CHECK(tiles.x == 0);
    CHECK(tiles.y == 0);
    CHECK(tiles.w == 1);
    CHECK(tiles.h == 1);

    // One pixel over the boundary has to take the neighbour with it, or that
    // pixel stays wrong on the guest.
    tiles = g.tiles_touching(Recti{63, 63, 2, 2});
    CHECK(tiles.x == 0);
    CHECK(tiles.y == 0);
    CHECK(tiles.w == 2);
    CHECK(tiles.h == 2);

    // Everything.
    tiles = g.tiles_touching(Recti{0, 0, 256, 256});
    CHECK(tiles.w == 4);
    CHECK(tiles.h == 4);
}

TEST_CASE("dirt outside the region is not dirt") {
    const ViewportGeometry g = make_geometry(Recti{100, 100, 200, 200}, 200, 200, 64);
    const Recti tiles = g.tiles_touching(Recti{0, 0, 50, 50});
    CHECK(tiles.w == 0);
    CHECK(tiles.h == 0);
}

TEST_CASE("dirt straddling the edge marks only the part inside") {
    const ViewportGeometry g = make_geometry(Recti{100, 100, 256, 256}, 256, 256, 64);
    const Recti tiles = g.tiles_touching(Recti{50, 50, 100, 100});
    // Overlaps the region from (100,100) to (150,150): the first tile only.
    CHECK(tiles.x == 0);
    CHECK(tiles.y == 0);
    CHECK(tiles.w == 1);
    CHECK(tiles.h == 1);
}

TEST_CASE("a scaled-down region still maps dirt to the right tiles") {
    // 1024 desktop pixels shown as 256: one output tile is 256 desktop pixels.
    const ViewportGeometry g = make_geometry(Recti{0, 0, 1024, 1024}, 256, 256, 64);
    CHECK(g.cols() == 4);

    const Recti tiles = g.tiles_touching(Recti{800, 800, 10, 10});
    CHECK(tiles.x == 3);
    CHECK(tiles.y == 3);
    CHECK(tiles.w == 1);
    CHECK(tiles.h == 1);
}

TEST_CASE("out of range tiles give nothing rather than reading past the end") {
    const ViewportGeometry g = make_geometry(Recti{0, 0, 128, 128}, 128, 128, 64);
    CHECK(g.tile_output_rect(-1).w == 0);
    CHECK(g.tile_output_rect(999).w == 0);
    CHECK(g.tile_source_rect(999).w == 0);
}
