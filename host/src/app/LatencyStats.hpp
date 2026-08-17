#pragma once

// Rolling latency window.
//
// An average alone hides the thing that actually matters for a digitizer: the
// occasional slow sample is what the hand feels, not the mean. So min, max and
// average are all kept, over a bounded window so an old spike eventually ages
// out instead of poisoning the display forever.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace digitiz::host {

class LatencyStats {
public:
    static constexpr std::size_t kWindow = 120;

    void add(double ms) {
        samples_[next_] = ms;
        next_ = (next_ + 1) % kWindow;
        if (filled_ < kWindow) {
            ++filled_;
        }
        last_ = ms;
        ++total_;
    }

    void reset() {
        next_ = 0;
        filled_ = 0;
        last_ = 0.0;
        total_ = 0;
    }

    bool empty() const noexcept { return filled_ == 0; }
    std::size_t size() const noexcept { return filled_; }
    std::uint64_t total() const noexcept { return total_; }
    double last() const noexcept { return last_; }

    double min() const noexcept {
        if (filled_ == 0) {
            return 0.0;
        }
        return *std::min_element(samples_.begin(), samples_.begin() + filled_);
    }

    double max() const noexcept {
        if (filled_ == 0) {
            return 0.0;
        }
        return *std::max_element(samples_.begin(), samples_.begin() + filled_);
    }

    double average() const noexcept {
        if (filled_ == 0) {
            return 0.0;
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < filled_; ++i) {
            sum += samples_[i];
        }
        return sum / static_cast<double>(filled_);
    }

private:
    std::array<double, kWindow> samples_{};
    std::size_t next_ = 0;
    std::size_t filled_ = 0;
    double last_ = 0.0;
    std::uint64_t total_ = 0;
};

// Estimates how far the guest's monotonic clock sits from the host's, so
// timestamps stamped on the phone can be compared against host time.
//
// Both sides read CLOCK_MONOTONIC but from different boots, so the offset is
// arbitrary and must be measured. The estimate comes from the round trip with
// the *lowest* RTT seen this session rather than an average: a fast round trip
// is close to symmetric, while a slow one has an unknown share of the delay on
// each leg and would drag an average off.
class ClockSync {
public:
    // All arguments in microseconds on their own clocks.
    void observe(std::uint64_t host_send_us, std::uint64_t guest_reply_us,
                 std::uint64_t host_recv_us) {
        if (host_recv_us < host_send_us) {
            return;
        }
        const std::uint64_t rtt = host_recv_us - host_send_us;
        if (have_ && rtt > best_rtt_us_) {
            return;
        }
        best_rtt_us_ = rtt;
        offset_us_ = static_cast<double>(guest_reply_us) -
                     (static_cast<double>(host_send_us) + static_cast<double>(rtt) / 2.0);
        have_ = true;
    }

    void reset() {
        have_ = false;
        best_rtt_us_ = 0;
        offset_us_ = 0.0;
    }

    bool ready() const noexcept { return have_; }
    double offset_us() const noexcept { return offset_us_; }
    std::uint64_t best_rtt_us() const noexcept { return best_rtt_us_; }

    // Converts a guest timestamp into host time.
    double to_host_us(std::uint64_t guest_us) const noexcept {
        return static_cast<double>(guest_us) - offset_us_;
    }

private:
    bool have_ = false;
    std::uint64_t best_rtt_us_ = 0;
    double offset_us_ = 0.0;
};

} // namespace digitiz::host
