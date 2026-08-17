#pragma once

// Bounded, thread-safe log buffer backing the console panel.
//
// Phase 3 logs from the transport RX thread while the UI thread draws, so the
// locking here is not optional.

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include <digitiz/core/log.hpp>

namespace digitiz::host {

struct LogEntry {
    core::LogLevel level = core::LogLevel::Info;
    double t_sec = 0.0; // seconds since process start
    std::string text;
};

class LogStore {
public:
    explicit LogStore(std::size_t capacity = 4000)
        : capacity_(capacity), start_(std::chrono::steady_clock::now()) {}

    void push(core::LogLevel level, std::string_view text) {
        const auto now = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(now - start_).count();

        std::lock_guard lock(mutex_);
        entries_.push_back(LogEntry{level, t, std::string(text)});
        while (entries_.size() > capacity_) {
            entries_.pop_front();
        }
        ++total_;
    }

    // Runs fn(const LogEntry&) over a consistent snapshot. The lock is held for
    // the duration, so keep fn cheap — drawing is fine, blocking is not.
    template <class F>
    void for_each(F&& fn) const {
        std::lock_guard lock(mutex_);
        for (const LogEntry& e : entries_) {
            fn(e);
        }
    }

    void clear() {
        std::lock_guard lock(mutex_);
        entries_.clear();
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

    std::uint64_t total_written() const {
        std::lock_guard lock(mutex_);
        return total_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<LogEntry> entries_;
    std::size_t capacity_;
    std::uint64_t total_ = 0;
    std::chrono::steady_clock::time_point start_;
};

} // namespace digitiz::host
