#include "screen/TileScheduler.hpp"

#include <algorithm>

namespace digitiz::host {

void TileScheduler::configure(int cols, int rows) {
    cols_ = std::max(cols, 0);
    rows_ = std::max(rows, 0);
    dirty_.assign(static_cast<std::size_t>(size()), false);
    dirty_count_ = 0;
    cursor_ = 0;
    focus_ = core::Recti{};
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

void TileScheduler::set_focus(core::Recti rect) {
    focus_ = rect;
}

bool TileScheduler::in_focus(int index) const noexcept {
    if (focus_.w <= 0 || focus_.h <= 0 || cols_ <= 0) {
        return false;
    }
    const int x = index % cols_;
    const int y = index / cols_;
    return x >= focus_.x && x < focus_.x + focus_.w && y >= focus_.y && y < focus_.y + focus_.h;
}

void TileScheduler::select(int budget, std::vector<std::uint16_t>& out) const {
    out.clear();
    if (budget <= 0 || dirty_count_ == 0) {
        return;
    }

    const int n = size();

    // The pen first. Not "always send" -- a clean tile has nothing to say, and
    // spending budget on one would come straight out of the tiles that do.
    for (int i = 0; i < n && static_cast<int>(out.size()) < budget; ++i) {
        if (dirty_[static_cast<std::size_t>(i)] && in_focus(i)) {
            out.push_back(static_cast<std::uint16_t>(i));
        }
    }

    // Then one lap of the sweep from wherever it stopped, skipping anything
    // the focus pass already took.
    for (int step = 0; step < n && static_cast<int>(out.size()) < budget; ++step) {
        const int i = (cursor_ + step) % n;
        if (!dirty_[static_cast<std::size_t>(i)] || in_focus(i)) {
            continue;
        }
        out.push_back(static_cast<std::uint16_t>(i));
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
