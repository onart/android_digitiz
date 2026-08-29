#include "screen/TileScheduler.hpp"

#include <algorithm>

namespace digitiz::host {

void TileScheduler::configure(int cols, int rows) {
    cols_ = std::max(cols, 0);
    rows_ = std::max(rows, 0);
    dirty_.assign(static_cast<std::size_t>(size()), false);
    dirty_count_ = 0;
    cursor_ = 0;
    focus_ = -1;
    // The guest has nothing for this grid yet, so all of it is owed.
    mark_all_dirty();
}

void TileScheduler::mark_all_dirty() {
    if (dirty_.empty()) {
        return;
    }
    dirty_.assign(dirty_.size(), true);
    dirty_count_ = size();
}

void TileScheduler::mark_dirty(int index) {
    if (!valid(index) || dirty_[static_cast<std::size_t>(index)]) {
        return;
    }
    dirty_[static_cast<std::size_t>(index)] = true;
    ++dirty_count_;
}

void TileScheduler::mark_dirty_rect(core::Recti rect) {
    const int x0 = std::max(rect.x, 0);
    const int y0 = std::max(rect.y, 0);
    const int x1 = std::min(rect.x + rect.w, cols_);
    const int y1 = std::min(rect.y + rect.h, rows_);

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            mark_dirty(y * cols_ + x);
        }
    }
}

void TileScheduler::set_focus(int index) noexcept {
    focus_ = valid(index) ? index : -1;
}

void TileScheduler::select(int budget, std::vector<std::uint16_t>& out) const {
    out.clear();
    if (size() == 0) {
        return;
    }

    // The pen's tile, on top of the budget and dirty or not. "Clean" only
    // means the guest has been sent what that tile last looked like, and the
    // place the stroke is landing is the one place worth paying for a repeat.
    if (valid(focus_)) {
        out.push_back(static_cast<std::uint16_t>(focus_));
    }

    if (budget <= 0 || dirty_count_ == 0) {
        return;
    }

    // Then one lap of the sweep from wherever it stopped.
    const int n = size();
    int taken = 0;
    for (int step = 0; step < n && taken < budget; ++step) {
        const int i = (cursor_ + step) % n;
        if (!dirty_[static_cast<std::size_t>(i)] || i == focus_) {
            continue;
        }
        out.push_back(static_cast<std::uint16_t>(i));
        ++taken;
    }
}

void TileScheduler::mark_sent(const std::vector<std::uint16_t>& tiles) {
    if (tiles.empty() || dirty_.empty()) {
        return;
    }

    const int n = size();
    int furthest = -1;

    for (const std::uint16_t tile : tiles) {
        const int index = static_cast<int>(tile);
        if (!valid(index)) {
            continue;
        }
        if (dirty_[static_cast<std::size_t>(index)]) {
            dirty_[static_cast<std::size_t>(index)] = false;
            --dirty_count_;
        }
        // The pen's tile is not part of the lap. It can be anywhere, and
        // letting it drag the cursor along would skip every dirty tile
        // between the sweep and wherever the hand happens to be.
        if (index == focus_) {
            continue;
        }
        // How far round the lap this tile sits, so the sweep resumes after the
        // last one taken rather than at the largest index -- the lap wraps.
        const int distance = (index - cursor_ + n) % n;
        furthest = std::max(furthest, distance);
    }

    if (furthest >= 0) {
        cursor_ = (cursor_ + furthest + 1) % n;
    }
}

} // namespace digitiz::host
