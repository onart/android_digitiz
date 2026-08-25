#pragma once

// Decides which tiles of the captured region go out this frame.
//
// The link cannot carry a full repaint at speed, so the frame budget is a
// fixed number of tiles and the scheduler spends it. That is the whole point:
// a hard cap turns "the screen changed too much" from a bandwidth blowout into
// a slower convergence, which is the one failure the design can afford. Input
// latency must never be the thing that gives.
//
// Two rules on top of the cap:
//
//   * Whatever the pen is near goes first. That is the part being looked at,
//     and it should never be the part that is behind.
//   * Everything else is a sweep, not a priority queue. A cursor walks the
//     grid and takes the dirty tiles it passes. Sweeping guarantees no tile
//     starves however often the rest of the screen changes, and it repaints in
//     reading order, which looks like a wipe rather than like static.
//
// The sweep is also what makes dropping safe. A dirty-tile scheme carries
// state -- a tile that is skipped stays wrong until it is sent, unlike a video
// frame that is merely late -- so without a guarantee that every dirty tile is
// eventually visited, a busy screen would leave permanent debris.

#include <cstdint>
#include <vector>

#include <digitiz/core/geometry.hpp>

namespace digitiz::host {

class TileScheduler {
public:
    // Grid size in tiles. Resizing forgets everything, since tile numbers no
    // longer mean the same places.
    void configure(int cols, int rows);

    int cols() const noexcept { return cols_; }
    int rows() const noexcept { return rows_; }
    int size() const noexcept { return cols_ * rows_; }

    // Every tile is dirty and nothing has been sent. Also the state after
    // configure(), because the guest has no picture yet.
    void mark_all_dirty();
    void mark_dirty(int index);
    // `rect` is in tile coordinates, clipped to the grid.
    void mark_dirty_rect(core::Recti rect);

    // Where the pen is, in tile coordinates. An empty rect clears it.
    void set_focus(core::Recti rect);

    // Up to `budget` tiles, focus first and then along the sweep. Does not
    // change any state: nothing is clean until it has actually gone out.
    void select(int budget, std::vector<std::uint16_t>& out) const;

    // Clears the dirty flags and advances the sweep past what was taken. Call
    // only for tiles that were really sent, so that a batch dropped on the way
    // to the socket is picked up again next time.
    void mark_sent(const std::vector<std::uint16_t>& tiles);

    int dirty_count() const noexcept { return dirty_count_; }
    bool anything_dirty() const noexcept { return dirty_count_ > 0; }
    bool dirty(int index) const noexcept {
        return valid(index) && dirty_[static_cast<std::size_t>(index)];
    }

private:
    bool valid(int index) const noexcept { return index >= 0 && index < size(); }
    bool in_focus(int index) const noexcept;

    int cols_ = 0;
    int rows_ = 0;
    std::vector<bool> dirty_;
    int dirty_count_ = 0;
    // Where the sweep resumes. Kept across frames, which is what spreads the
    // budget over the whole grid instead of replaying the same tiles.
    int cursor_ = 0;
    core::Recti focus_{};
};

} // namespace digitiz::host
