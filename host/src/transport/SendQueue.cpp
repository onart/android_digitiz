#include "transport/SendQueue.hpp"

#include <algorithm>

namespace digitiz::host {

void SendQueue::push_interactive(std::vector<std::byte> message) {
    if (message.empty()) {
        return;
    }
    interactive_.push_back(std::move(message));
}

void SendQueue::push_bulk(std::vector<std::byte> message) {
    if (message.empty()) {
        return;
    }
    if (message.size() > bulk_limit_) {
        // Not refused: dropping screen data silently would be worse than
        // sending it late. Counted, because it means an interactive message
        // can now be stuck for longer than the chunk size promised.
        ++oversize_bulk_;
    }
    bulk_bytes_ += message.size();
    bulk_.push_back(std::move(message));
}

void SendQueue::take_next() {
    // Interactive first, always. The only thing that outranks it is a message
    // already partly written, and that is handled by not getting here.
    if (!interactive_.empty()) {
        current_ = std::move(interactive_.front());
        interactive_.pop_front();
        current_is_bulk_ = false;
    } else if (!bulk_.empty()) {
        current_ = std::move(bulk_.front());
        bulk_.pop_front();
        bulk_bytes_ -= current_.size();
        current_is_bulk_ = true;
    } else {
        current_.clear();
        current_is_bulk_ = false;
    }
    offset_ = 0;
}

std::span<const std::byte> SendQueue::pending(std::size_t max_bytes) {
    if (!in_flight()) {
        take_next();
    }
    if (!in_flight() || max_bytes == 0) {
        return {};
    }
    const std::size_t remaining = current_.size() - offset_;
    return std::span<const std::byte>(current_.data() + offset_, std::min(max_bytes, remaining));
}

void SendQueue::consume(std::size_t bytes) {
    if (!in_flight()) {
        return;
    }
    offset_ = std::min(offset_ + bytes, current_.size());
    if (offset_ >= current_.size()) {
        current_.clear();
        offset_ = 0;
        current_is_bulk_ = false;
    }
}

bool SendQueue::empty() const noexcept {
    return interactive_.empty() && bulk_.empty() && !in_flight();
}

bool SendQueue::bulk_idle() const noexcept {
    return bulk_.empty() && !(current_is_bulk_ && in_flight());
}

std::size_t SendQueue::inversion_bytes() const noexcept {
    return in_flight() ? current_.size() - offset_ : 0;
}

std::size_t SendQueue::drop_queued_bulk() {
    const std::size_t dropped = bulk_.size();
    bulk_.clear();
    bulk_bytes_ = 0;
    return dropped;
}

void SendQueue::clear() {
    interactive_.clear();
    bulk_.clear();
    bulk_bytes_ = 0;
    current_.clear();
    offset_ = 0;
    current_is_bulk_ = false;
}

} // namespace digitiz::host
