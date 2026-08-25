#include <doctest/doctest.h>

#include <algorithm>
#include <set>

#include "screen/TileScheduler.hpp"

using namespace digitiz::host;

namespace {

std::vector<std::uint16_t> take(TileScheduler& s, int budget) {
    std::vector<std::uint16_t> picked;
    s.select(budget, picked);
    s.mark_sent(picked);
    return picked;
}

std::vector<int> as_ints(const std::vector<std::uint16_t>& v) {
    return std::vector<int>(v.begin(), v.end());
}

} // namespace

TEST_CASE("a fresh grid owes the guest everything") {
    TileScheduler s;
    s.configure(4, 3);
    CHECK(s.size() == 12);
    // The guest has no picture for this grid yet, so nothing is clean.
    CHECK(s.dirty_count() == 12);
}

TEST_CASE("the budget is a hard cap, whatever the screen is doing") {
    TileScheduler s;
    s.configure(10, 10);
    std::vector<std::uint16_t> picked;

    s.select(7, picked);
    CHECK(picked.size() == 7);

    s.select(0, picked);
    CHECK(picked.empty());

    // Asking for more than is owed gives what is owed, not padding.
    TileScheduler small;
    small.configure(2, 1);
    small.select(50, picked);
    CHECK(picked.size() == 2);
}

TEST_CASE("the sweep converges in ceil(n / budget) frames and covers everything") {
    TileScheduler s;
    s.configure(5, 2); // 10 tiles

    std::set<int> seen;
    int frames = 0;
    while (s.anything_dirty()) {
        for (const int i : as_ints(take(s, 3))) {
            seen.insert(i);
        }
        ++frames;
        REQUIRE(frames < 20); // a sweep that cannot finish is the bug this guards
    }

    CHECK(frames == 4); // ceil(10 / 3)
    CHECK(seen.size() == 10);
}

TEST_CASE("the sweep resumes where it stopped rather than restarting") {
    TileScheduler s;
    s.configure(10, 1);

    CHECK(as_ints(take(s, 3)) == std::vector<int>{0, 1, 2});
    CHECK(as_ints(take(s, 3)) == std::vector<int>{3, 4, 5});
    CHECK(as_ints(take(s, 3)) == std::vector<int>{6, 7, 8});
    CHECK(as_ints(take(s, 3)) == std::vector<int>{9});
}

TEST_CASE("the sweep wraps, so a tile behind the cursor is still reached") {
    TileScheduler s;
    s.configure(10, 1);
    take(s, 10); // clean, cursor back at 0

    // Push the cursor along, then dirty something it has already passed.
    s.mark_dirty(7);
    CHECK(as_ints(take(s, 4)) == std::vector<int>{7});

    s.mark_dirty(2);
    CHECK(as_ints(take(s, 4)) == std::vector<int>{2});
}

TEST_CASE("nothing starves while one tile keeps changing") {
    TileScheduler s;
    s.configure(8, 1);
    take(s, 8); // start from a clean grid

    std::set<int> seen;
    for (int frame = 0; frame < 8; ++frame) {
        // A caret blinking, or a clock: one tile dirty every single frame.
        s.mark_dirty(0);
        s.mark_dirty(frame);
        for (const int i : as_ints(take(s, 1))) {
            seen.insert(i);
        }
    }

    // With a priority queue keyed on that tile it would be the only one ever
    // sent. The sweep is what makes the rest reachable.
    CHECK(seen.size() > 1);
}

TEST_CASE("the pen's tiles go first, wherever the sweep happens to be") {
    TileScheduler s;
    s.configure(10, 10);
    // Bottom-right corner, a long way round the lap from the cursor at 0.
    s.set_focus(digitiz::core::Recti{8, 9, 2, 1});

    std::vector<std::uint16_t> picked;
    s.select(4, picked);
    REQUIRE(picked.size() == 4);

    const std::vector<int> got = as_ints(picked);
    CHECK(got[0] == 98);
    CHECK(got[1] == 99);
    // The rest of the budget goes to the sweep, which is still at the start.
    CHECK(got[2] == 0);
    CHECK(got[3] == 1);
}

TEST_CASE("a clean focus costs nothing") {
    TileScheduler s;
    s.configure(4, 4);
    take(s, 16);

    s.set_focus(digitiz::core::Recti{0, 0, 2, 2});
    s.mark_dirty(15);

    // Spending budget on tiles with nothing to say would come straight out of
    // the tiles that have something to say.
    std::vector<std::uint16_t> picked;
    s.select(4, picked);
    CHECK(as_ints(picked) == std::vector<int>{15});
}

TEST_CASE("focus is clamped by the grid, not trusted") {
    TileScheduler s;
    s.configure(3, 3);
    s.set_focus(digitiz::core::Recti{-5, -5, 100, 100});

    std::vector<std::uint16_t> picked;
    s.select(9, picked);
    CHECK(picked.size() == 9);
}

TEST_CASE("selecting decides nothing; only sending does") {
    TileScheduler s;
    s.configure(6, 1);

    std::vector<std::uint16_t> first;
    std::vector<std::uint16_t> second;
    s.select(2, first);
    s.select(2, second);

    // A batch that never reached the socket must come back, or the tiles it
    // held stay wrong on the guest forever -- tiles carry state, unlike a
    // video frame that is merely late.
    CHECK(first == second);
    CHECK(s.dirty_count() == 6);

    s.mark_sent(first);
    CHECK(s.dirty_count() == 4);
}

TEST_CASE("a tile that changes again after being sent is owed again") {
    TileScheduler s;
    s.configure(4, 1);
    take(s, 4);
    CHECK_FALSE(s.anything_dirty());

    s.mark_dirty(2);
    CHECK(s.dirty(2));
    CHECK(s.dirty_count() == 1);

    // Marking it twice does not owe it twice.
    s.mark_dirty(2);
    CHECK(s.dirty_count() == 1);
}

TEST_CASE("a dirty rectangle is clipped to the grid") {
    TileScheduler s;
    s.configure(4, 4);
    take(s, 16);

    s.mark_dirty_rect(digitiz::core::Recti{2, 2, 10, 10});
    CHECK(s.dirty_count() == 4); // the 2x2 corner, not the 10x10 asked for

    s.mark_dirty_rect(digitiz::core::Recti{-3, -3, 4, 4});
    CHECK(s.dirty(0));
    CHECK_FALSE(s.dirty(2));

    // Entirely outside is not an error, just nothing.
    const int before = s.dirty_count();
    s.mark_dirty_rect(digitiz::core::Recti{40, 40, 2, 2});
    CHECK(s.dirty_count() == before);
}

TEST_CASE("indices from elsewhere do not corrupt the count") {
    TileScheduler s;
    s.configure(3, 1);
    s.mark_dirty(-1);
    s.mark_dirty(99);
    CHECK(s.dirty_count() == 3);

    s.mark_sent(std::vector<std::uint16_t>{99, 0});
    CHECK(s.dirty_count() == 2);
}

TEST_CASE("regridding forgets the old numbering") {
    TileScheduler s;
    s.configure(4, 4);
    take(s, 16);
    CHECK_FALSE(s.anything_dirty());

    // Tile 5 of a 4x4 grid is not tile 5 of an 8x8 one, so none of what the
    // guest has can be reused.
    s.configure(8, 8);
    CHECK(s.size() == 64);
    CHECK(s.dirty_count() == 64);
}

TEST_CASE("an empty grid is inert rather than a crash") {
    TileScheduler s;
    s.configure(0, 0);
    CHECK(s.size() == 0);
    CHECK_FALSE(s.anything_dirty());

    std::vector<std::uint16_t> picked;
    s.select(4, picked);
    CHECK(picked.empty());
    s.mark_sent(std::vector<std::uint16_t>{0, 1});
    CHECK(s.dirty_count() == 0);
}
