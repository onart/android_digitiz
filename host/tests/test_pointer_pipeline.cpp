#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "input/PointerPipeline.hpp"

using namespace digitiz;
using namespace digitiz::host;

namespace {

// Records what the pipeline asked the OS to do, so the tests can assert on the
// exact sequence rather than on internal state.
class FakeInjector final : public IInputInjector {
public:
    std::vector<std::string> calls;
    bool fail_next = false;
    int release_all_count = 0;

    void set_virtual_bounds(core::Recti b) override { bounds_ = b; }
    core::Recti virtual_bounds() const override { return bounds_; }

    bool move_to(std::int32_t x, std::int32_t y) override {
        if (take_failure()) {
            return false;
        }
        calls.push_back("move " + std::to_string(x) + "," + std::to_string(y));
        return true;
    }

    bool button(proto::MouseButton b, bool down, std::int32_t x, std::int32_t y) override {
        if (take_failure()) {
            return false;
        }
        calls.push_back(std::string(down ? "down " : "up ") + proto::to_string(b) + " " +
                        std::to_string(x) + "," + std::to_string(y));
        held_[static_cast<std::size_t>(b)] = down;
        return true;
    }

    bool key(const proto::Key& k) override {
        if (take_failure()) {
            return false;
        }
        std::string line = "key ";
        if ((k.modifiers & proto::kModCtrl) != 0) {
            line += "ctrl+";
        }
        if ((k.modifiers & proto::kModShift) != 0) {
            line += "shift+";
        }
        if ((k.modifiers & proto::kModAlt) != 0) {
            line += "alt+";
        }
        line += k.key;
        calls.push_back(line);
        return true;
    }

    void release_all() override {
        ++release_all_count;
        for (bool& h : held_) {
            h = false;
        }
    }

    bool any_button_down() const override {
        return held_[0] || held_[1] || held_[2];
    }

    // Where the OS thinks the cursor is. Relative mode reads this to seed
    // itself, so tests can pretend the user moved a physical mouse.
    std::int32_t cursor_x = 0;
    std::int32_t cursor_y = 0;
    bool cursor_known = true;

    bool cursor_pos(std::int32_t& x, std::int32_t& y) const override {
        if (!cursor_known) {
            return false;
        }
        x = cursor_x;
        y = cursor_y;
        return true;
    }

    std::uint64_t clamped_count() const override { return 0; }

private:
    bool take_failure() {
        if (!fail_next) {
            return false;
        }
        fail_next = false;
        return true;
    }

    core::Recti bounds_{0, 0, 1920, 1080};
    bool held_[3] = {false, false, false};
};

proto::Pointer ev(proto::PointerAction a, std::int32_t x, std::int32_t y,
                  proto::MouseButton b = proto::MouseButton::Left) {
    proto::Pointer p;
    p.action = a;
    p.x = x;
    p.y = y;
    p.button = b;
    return p;
}

} // namespace

TEST_CASE("the pipeline starts disabled and injects nothing") {
    FakeInjector inj;
    PointerPipeline pipe(inj);

    CHECK_FALSE(pipe.enabled());

    pipe.handle(ev(proto::PointerAction::Down, 10, 20));
    pipe.handle(ev(proto::PointerAction::Move, 11, 21));
    pipe.handle(ev(proto::PointerAction::Up, 12, 22));

    CHECK(inj.calls.empty());
    CHECK(pipe.stats().received == 3);
    CHECK(pipe.stats().dropped_disabled == 3);
    CHECK(pipe.stats().injected == 0);
}

TEST_CASE("a clean stroke becomes down / move / up") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 100, 200));
    CHECK(pipe.stroke_active());
    pipe.handle(ev(proto::PointerAction::Move, 110, 210));
    pipe.handle(ev(proto::PointerAction::Up, 120, 220));
    CHECK_FALSE(pipe.stroke_active());

    REQUIRE(inj.calls.size() == 3);
    CHECK(inj.calls[0] == "down LEFT 100,200");
    CHECK(inj.calls[1] == "move 110,210");
    CHECK(inj.calls[2] == "up LEFT 120,220");
    CHECK(pipe.stats().protocol_errors == 0);
}

TEST_CASE("a move outside a stroke still moves the cursor") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Move, 5, 6));

    REQUIRE(inj.calls.size() == 1);
    CHECK(inj.calls[0] == "move 5,6");
    CHECK_FALSE(pipe.stroke_active());
}

TEST_CASE("a duplicate DOWN closes the orphaned stroke first") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 10, 10));
    pipe.handle(ev(proto::PointerAction::Down, 20, 20));

    REQUIRE(inj.calls.size() == 3);
    CHECK(inj.calls[0] == "down LEFT 10,10");
    CHECK(inj.calls[1] == "up LEFT 20,20"); // previous stroke closed
    CHECK(inj.calls[2] == "down LEFT 20,20");
    CHECK(pipe.stats().protocol_errors == 1);
    CHECK(pipe.stroke_active());
}

TEST_CASE("an UP with no stroke open is ignored") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Up, 1, 2));

    CHECK(inj.calls.empty());
    CHECK(pipe.stats().protocol_errors == 1);
    CHECK_FALSE(inj.any_button_down());
}

TEST_CASE("an UP naming the wrong button releases the one actually held") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 0, 0, proto::MouseButton::Right));
    pipe.handle(ev(proto::PointerAction::Up, 0, 0, proto::MouseButton::Left));

    REQUIRE(inj.calls.size() == 2);
    CHECK(inj.calls[1] == "up RIGHT 0,0");
    CHECK(pipe.stats().protocol_errors == 1);
    CHECK_FALSE(inj.any_button_down());
}

TEST_CASE("CANCEL ends the stroke — the second finger landed") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 50, 60));
    pipe.handle(ev(proto::PointerAction::Move, 55, 65));
    pipe.handle(ev(proto::PointerAction::Cancel, 57, 67));

    REQUIRE(inj.calls.size() == 3);
    CHECK(inj.calls[2] == "up LEFT 57,67");
    CHECK_FALSE(pipe.stroke_active());
    CHECK_FALSE(inj.any_button_down());
    CHECK(pipe.stats().protocol_errors == 0); // cancel is normal, not an error
}

TEST_CASE("CANCEL with no stroke open does nothing") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Cancel, 1, 1));

    CHECK(inj.calls.empty());
    CHECK(pipe.stats().protocol_errors == 0);
}

TEST_CASE("disabling mid-stroke releases the held button") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 300, 400));
    REQUIRE(inj.any_button_down());

    pipe.set_enabled(false);

    REQUIRE(inj.calls.size() == 2);
    CHECK(inj.calls[1] == "up LEFT 300,400");
    CHECK_FALSE(inj.any_button_down());
    CHECK_FALSE(pipe.stroke_active());
}

TEST_CASE("end_session releases the stroke and sweeps the injector") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 7, 8, proto::MouseButton::Middle));
    pipe.end_session();

    REQUIRE(inj.calls.size() == 2);
    CHECK(inj.calls[1] == "up MIDDLE 7,8");
    CHECK(inj.release_all_count == 1);
    CHECK_FALSE(inj.any_button_down());
}

TEST_CASE("end_session is safe with nothing held") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.end_session();

    CHECK(inj.calls.empty());
    CHECK(inj.release_all_count == 1);
}

// A 1920x1080 primary with a shorter 1280x1024 monitor beside it. The virtual
// bounding box is 3200x1080, so the strip below the second screen is inside
// the box but on no display — the case a bounds check would get wrong.
namespace {

bool two_monitor_layout(std::int32_t x, std::int32_t y) {
    const bool primary = x >= 0 && x < 1920 && y >= 0 && y < 1080;
    const bool second = x >= 1920 && x < 3200 && y >= 0 && y < 1024;
    return primary || second;
}

} // namespace

TEST_CASE("a DOWN on no display is ignored rather than clamped") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_screen_test(&two_monitor_layout);
    pipe.set_enabled(true);

    // Inside the virtual bounding box, below the shorter second monitor.
    pipe.handle(ev(proto::PointerAction::Down, 2500, 1050));

    CHECK(inj.calls.empty());
    CHECK(pipe.stats().dropped_off_screen == 1);
    CHECK_FALSE(pipe.stroke_active());
}

TEST_CASE("a stroke survives crossing the dead corner between monitors") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_screen_test(&two_monitor_layout);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 1000, 1050)); // on the primary
    pipe.handle(ev(proto::PointerAction::Move, 2500, 1050)); // in the dead strip
    pipe.handle(ev(proto::PointerAction::Move, 2500, 500));  // on the second
    pipe.handle(ev(proto::PointerAction::Up, 2500, 500));

    REQUIRE(inj.calls.size() == 3);
    CHECK(inj.calls[0] == "down LEFT 1000,1050");
    CHECK(inj.calls[1] == "move 2500,500"); // the dead point never moved it
    CHECK(inj.calls[2] == "up LEFT 2500,500");
    CHECK(pipe.stats().dropped_off_screen == 1);
}

TEST_CASE("an UP off-screen still releases") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_screen_test(&two_monitor_layout);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 100, 100));
    pipe.handle(ev(proto::PointerAction::Up, 2500, 1050)); // nowhere

    REQUIRE(inj.calls.size() == 2);
    CHECK(inj.calls[1] == "up LEFT 2500,1050");
    CHECK_FALSE(inj.any_button_down());
    CHECK_FALSE(pipe.stroke_active());
}

TEST_CASE("a CANCEL off-screen still releases") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_screen_test(&two_monitor_layout);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 100, 100));
    pipe.handle(ev(proto::PointerAction::Cancel, 3199, 1079));

    CHECK_FALSE(inj.any_button_down());
    CHECK_FALSE(pipe.stroke_active());
}

TEST_CASE("without a screen test nothing is dropped") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, -99999, 99999));

    REQUIRE(inj.calls.size() == 1);
    CHECK(pipe.stats().dropped_off_screen == 0);
}

TEST_CASE("smoothing fills a stroke in and still ends on the last sample") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.smoother().set_enabled(true);
    pipe.smoother().set_step_px(2.0);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 0, 0));
    pipe.handle(ev(proto::PointerAction::Move, 40, 10));
    pipe.handle(ev(proto::PointerAction::Move, 80, 0));
    pipe.handle(ev(proto::PointerAction::Up, 120, 10));

    // Four samples in, many more moves out.
    CHECK(pipe.stats().smoothed_points > 20);
    CHECK(inj.calls.front() == "down LEFT 0,0");
    CHECK(inj.calls.back() == "up LEFT 120,10");
    CHECK_FALSE(inj.any_button_down());
}

TEST_CASE("smoothing off leaves the stroke exactly as before") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 0, 0));
    pipe.handle(ev(proto::PointerAction::Move, 40, 10));
    pipe.handle(ev(proto::PointerAction::Up, 80, 0));

    REQUIRE(inj.calls.size() == 3);
    CHECK(inj.calls[1] == "move 40,10");
    CHECK(pipe.stats().smoothed_points == 0);
}

TEST_CASE("a cancelled stroke does not get drained onto the screen") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.smoother().set_enabled(true);
    pipe.set_enabled(true);

    pipe.handle(ev(proto::PointerAction::Down, 0, 0));
    pipe.handle(ev(proto::PointerAction::Move, 40, 0));
    const std::uint64_t before = pipe.stats().smoothed_points;

    pipe.handle(ev(proto::PointerAction::Cancel, 60, 0));

    CHECK(pipe.stats().smoothed_points == before);
    CHECK_FALSE(inj.any_button_down());
}

// --- slide (relative) mode -------------------------------------------------

namespace {

proto::Pointer rel(proto::PointerAction a, std::int32_t dx, std::int32_t dy, bool start = false) {
    proto::Pointer p = ev(a, dx, dy);
    p.flags = proto::kPointerRelative;
    if (start) {
        p.flags |= proto::kPointerGestureStart;
    }
    return p;
}

} // namespace

TEST_CASE("a relative move continues from where the cursor already is") {
    FakeInjector inj;
    inj.cursor_x = 800;
    inj.cursor_y = 400;

    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(rel(proto::PointerAction::Move, 10, -5, true));
    pipe.handle(rel(proto::PointerAction::Move, 10, -5));

    REQUIRE(inj.calls.size() == 2);
    CHECK(inj.calls[0] == "move 810,395");
    CHECK(inj.calls[1] == "move 820,390");
    CHECK(pipe.stats().relative_events == 2);
}

TEST_CASE("a moving cursor does not press anything") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(rel(proto::PointerAction::Move, 5, 5, true));
    pipe.handle(rel(proto::PointerAction::Up, 0, 0));

    CHECK_FALSE(inj.any_button_down());
    // The UP with nothing held is expected here, not a protocol error.
    CHECK(pipe.stats().protocol_errors == 0);
}

TEST_CASE("a tap clicks wherever the cursor is") {
    FakeInjector inj;
    inj.cursor_x = 640;
    inj.cursor_y = 480;

    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(rel(proto::PointerAction::Down, 0, 0, true));
    pipe.handle(rel(proto::PointerAction::Up, 0, 0));

    REQUIRE(inj.calls.size() == 2);
    CHECK(inj.calls[0] == "down LEFT 640,480");
    CHECK(inj.calls[1] == "up LEFT 640,480");
    CHECK_FALSE(inj.any_button_down());
}

TEST_CASE("each gesture re-reads the cursor, so a physical mouse move is not fought") {
    FakeInjector inj;
    inj.cursor_x = 100;
    inj.cursor_y = 100;

    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(rel(proto::PointerAction::Move, 50, 0, true));
    CHECK(inj.calls.back() == "move 150,100");

    // The user grabs the mouse and drags the cursor elsewhere.
    inj.cursor_x = 900;
    inj.cursor_y = 700;

    pipe.handle(rel(proto::PointerAction::Move, 10, 10, true));
    CHECK(inj.calls.back() == "move 910,710");
}

TEST_CASE("without the start flag the accumulator carries on") {
    FakeInjector inj;
    inj.cursor_x = 200;
    inj.cursor_y = 200;

    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    pipe.handle(rel(proto::PointerAction::Move, 5, 0, true));
    // A stale cursor reading must not be used mid-gesture: injected moves are
    // asynchronous, so the OS may not have caught up yet.
    inj.cursor_x = 0;
    inj.cursor_y = 0;
    pipe.handle(rel(proto::PointerAction::Move, 5, 0));

    CHECK(inj.calls.back() == "move 210,200");
}

TEST_CASE("relative movement stops at the screen edge and slides along it") {
    FakeInjector inj;
    inj.cursor_x = 1900;
    inj.cursor_y = 500;

    PointerPipeline pipe(inj);
    pipe.set_screen_test(&two_monitor_layout);
    pipe.set_enabled(true);

    // Diagonally into the far right edge of the second monitor's row: x is
    // refused, y should still take effect.
    pipe.handle(rel(proto::PointerAction::Move, 5000, 100, true));

    REQUIRE(inj.calls.size() == 1);
    CHECK(inj.calls[0] == "move 1900,600");
}

TEST_CASE("the tracked position is clamped, not just the injected one") {
    FakeInjector inj;
    inj.cursor_x = 10;
    inj.cursor_y = 500;

    PointerPipeline pipe(inj);
    pipe.set_screen_test(&two_monitor_layout);
    pipe.set_enabled(true);

    // Shove far past the left edge, then come back a little. If only the
    // injected point were clamped, the accumulator would sit at -990 and this
    // would have to unwind before the cursor moved at all.
    pipe.handle(rel(proto::PointerAction::Move, -1000, 0, true));
    pipe.handle(rel(proto::PointerAction::Move, 30, 0));

    CHECK(inj.calls.back() == "move 40,500");
}

TEST_CASE("relative events are dropped while injection is off") {
    FakeInjector inj;
    PointerPipeline pipe(inj);

    pipe.handle(rel(proto::PointerAction::Move, 10, 10, true));

    CHECK(inj.calls.empty());
    CHECK(pipe.stats().dropped_disabled == 1);
}

TEST_CASE("a failed injection is counted, not silently swallowed") {
    FakeInjector inj;
    PointerPipeline pipe(inj);
    pipe.set_enabled(true);

    inj.fail_next = true;
    pipe.handle(ev(proto::PointerAction::Move, 1, 1));

    CHECK(inj.calls.empty());
    CHECK(pipe.stats().inject_failures == 1);
    CHECK(pipe.stats().injected == 0);
}

TEST_CASE("re-enabling after a disabled stretch resumes cleanly") {
    FakeInjector inj;
    PointerPipeline pipe(inj);

    pipe.handle(ev(proto::PointerAction::Down, 1, 1)); // dropped, still disabled
    pipe.set_enabled(true);
    pipe.handle(ev(proto::PointerAction::Down, 2, 2));
    pipe.handle(ev(proto::PointerAction::Up, 3, 3));

    REQUIRE(inj.calls.size() == 2);
    CHECK(inj.calls[0] == "down LEFT 2,2");
    CHECK(inj.calls[1] == "up LEFT 3,3");
    CHECK(pipe.stats().dropped_disabled == 1);
    CHECK(pipe.stats().protocol_errors == 0);
}


TEST_CASE("shortcuts honour the on/off switch") {
    FakeInjector inj;
    PointerPipeline p(inj);

    p.handle(proto::Key{.modifiers = proto::kModCtrl, .key = "s"});
    CHECK(inj.calls.empty());
    CHECK(p.stats().dropped_disabled == 1);

    p.set_enabled(true);
    p.handle(proto::Key{.modifiers = proto::kModCtrl | proto::kModShift, .key = "s"});
    REQUIRE(inj.calls.size() == 1);
    CHECK(inj.calls[0] == "key ctrl+shift+s");
    CHECK(p.stats().keys_sent == 1);
}

TEST_CASE("a key the injector refuses is counted, not retried") {
    FakeInjector inj;
    PointerPipeline p(inj);
    p.set_enabled(true);

    inj.fail_next = true;
    p.handle(proto::Key{.key = "banana"});
    CHECK(inj.calls.empty());
    CHECK(p.stats().keys_unknown == 1);
    CHECK(p.stats().keys_sent == 0);
}
