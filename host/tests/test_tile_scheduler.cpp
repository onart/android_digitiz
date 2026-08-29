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

TEST_CASE("the pen's tile rides on top of the budget, not inside it") {
    TileScheduler s;
    s.configure(10, 10);
    // Bottom-right corner, a long way round the lap from the cursor at 0.
    s.set_focus(98);

    std::vector<std::uint16_t> picked;
    s.select(4, picked);

    // Four sweep tiles AND the pen's, not three sweep tiles and the pen's.
    // Inside the budget the two would take from each other, and the rest of
    // the screen would converge slower exactly while it is being drawn on.
    REQUIRE(picked.size() == 5);

    const std::vector<int> got = as_ints(picked);
    CHECK(got[0] == 98);
    CHECK(got[1] == 0);
    CHECK(got[2] == 1);
    CHECK(got[3] == 2);
    CHECK(got[4] == 3);
}

TEST_CASE("the pen's tile goes even when it is clean") {
    TileScheduler s;
    s.configure(4, 4);
    take(s, 16); // everything sent, nothing dirty
    REQUIRE(s.dirty_count() == 0);

    s.set_focus(5);
    std::vector<std::uint16_t> picked;
    s.select(4, picked);

    // The stroke is landing there. "Clean" only means the guest has what that
    // tile looked like a moment ago, which is precisely what is out of date.
    CHECK(as_ints(picked) == std::vector<int>{5});
}

TEST_CASE("the pen does not drag the sweep along with it") {
    TileScheduler s;
    s.configure(10, 10);
    s.set_focus(98);

    std::vector<std::uint16_t> picked;
    s.select(2, picked);
    s.mark_sent(picked);

    // The cursor must sit just past the two sweep tiles, not past tile 98.
    // Otherwise one touch in the corner would declare ninety-odd dirty tiles
    // skipped, and they would stay wrong until the next lap.
    s.select(2, picked);
    const std::vector<int> got = as_ints(picked);
    REQUIRE(got.size() == 3);
    CHECK(got[0] == 98);
    CHECK(got[1] == 2);
    CHECK(got[2] == 3);
}

TEST_CASE("a focus off the grid is no focus") {
    TileScheduler s;
    s.configure(3, 3);
    s.set_focus(99);
    CHECK(s.focus() == -1);

    s.set_focus(-1);
    CHECK(s.focus() == -1);

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
