#include <doctest/doctest.h>

#include "app/LatencyStats.hpp"

using namespace digitiz::host;

TEST_CASE("an empty window reports zeroes rather than garbage") {
    LatencyStats s;
    CHECK(s.empty());
    CHECK(s.min() == doctest::Approx(0.0));
    CHECK(s.max() == doctest::Approx(0.0));
    CHECK(s.average() == doctest::Approx(0.0));
}

TEST_CASE("min, max and average track the samples") {
    LatencyStats s;
    s.add(4.0);
    s.add(2.0);
    s.add(9.0);

    CHECK(s.size() == 3);
    CHECK(s.last() == doctest::Approx(9.0));
    CHECK(s.min() == doctest::Approx(2.0));
    CHECK(s.max() == doctest::Approx(9.0));
    CHECK(s.average() == doctest::Approx(5.0));
    CHECK(s.total() == 3);
}

TEST_CASE("an old spike ages out of the window") {
    LatencyStats s;
    s.add(500.0); // the spike

    for (std::size_t i = 0; i < LatencyStats::kWindow; ++i) {
        s.add(3.0);
    }

    CHECK(s.size() == LatencyStats::kWindow);
    CHECK(s.max() == doctest::Approx(3.0)); // spike gone
    CHECK(s.total() == LatencyStats::kWindow + 1);
}

TEST_CASE("the window fills exactly once and then rotates") {
    LatencyStats s;
    for (std::size_t i = 0; i < LatencyStats::kWindow * 3; ++i) {
        s.add(1.0);
    }
    CHECK(s.size() == LatencyStats::kWindow);
}

TEST_CASE("reset clears the window but the counter starts over too") {
    LatencyStats s;
    s.add(7.0);
    s.reset();
    CHECK(s.empty());
    CHECK(s.total() == 0);
}

// ---------------------------------------------------------------------------

TEST_CASE("clock sync recovers a known offset from a symmetric round trip") {
    ClockSync sync;
    CHECK_FALSE(sync.ready());

    // Host sends at 1000, round trip takes 40, so the guest replied at host
    // time 1020. Its own clock reads 51020, i.e. 50000 ahead.
    sync.observe(1000, 51020, 1040);

    REQUIRE(sync.ready());
    CHECK(sync.offset_us() == doctest::Approx(50000.0));
    CHECK(sync.best_rtt_us() == 40);

    // A guest timestamp converts back to host time.
    CHECK(sync.to_host_us(51040) == doctest::Approx(1040.0));
}

TEST_CASE("clock sync prefers the fastest round trip and ignores slower ones") {
    ClockSync sync;

    // A slow, lopsided round trip first: it would put the offset at 49500.
    sync.observe(1000, 50500, 3000);
    CHECK(sync.offset_us() == doctest::Approx(48500.0));

    // A fast one lands closer to the truth and must win.
    sync.observe(10000, 60020, 10040);
    CHECK(sync.best_rtt_us() == 40);
    CHECK(sync.offset_us() == doctest::Approx(50000.0));

    // A later slow sample must not disturb it.
    sync.observe(20000, 70900, 22000);
    CHECK(sync.offset_us() == doctest::Approx(50000.0));
}

TEST_CASE("clock sync ignores a round trip that appears to travel backwards") {
    ClockSync sync;
    sync.observe(5000, 1234, 4000); // recv before send: nonsense
    CHECK_FALSE(sync.ready());
}

TEST_CASE("clock sync resets per session") {
    ClockSync sync;
    sync.observe(1000, 51020, 1040);
    REQUIRE(sync.ready());

    sync.reset();
    CHECK_FALSE(sync.ready());

    // A slow round trip is accepted again once there is no better sample.
    sync.observe(1000, 50500, 3000);
    CHECK(sync.ready());
    CHECK(sync.best_rtt_us() == 2000);
}
