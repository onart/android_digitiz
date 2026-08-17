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

    void release_all() override {
        ++release_all_count;
        for (bool& h : held_) {
            h = false;
        }
    }

    bool any_button_down() const override {
        return held_[0] || held_[1] || held_[2];
    }

    bool cursor_pos(std::int32_t&, std::int32_t&) const override { return false; }

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
