#pragma once

// Where the desktop image comes from, and — just as importantly — what changed
// in it.
//
// The whole transfer design rests on the changed regions being small most of
// the time, so a source that could only say "here is a frame" would be no use.
// Every backend has a way to say more: DXGI has dirty and move rects, X11 has
// XDamage, and a PipeWire stream carries damage regions too.
//
// Move rects are in the interface because DXGI has a field for them, not
// because Windows fills it in. Measured with --capture-test across an idle
// desktop, a text window scrolled continuously, and a window dragged across
// the screen: zero, every time. DWM composites rather than blitting, so
// nothing ever looks like a slide to the compositor. Scrolling really is a
// full-window repaint, and the tile budget is what has to absorb it.
//
// Kept anyway: other backends report damage differently, and a field nobody
// fills costs nothing.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <digitiz/core/geometry.hpp>

namespace digitiz::host {

// A block of pixels the compositor slid from one place to another. The guest
// can do the same to its copy and be told nothing else about it.
struct MoveRect {
    core::Recti to{};
    std::int32_t from_x = 0;
    std::int32_t from_y = 0;
};

struct FrameUpdate {
    std::uint64_t frame = 0;
    // Nothing useful was said about what changed, so all of it has to be
    // assumed changed: the first frame, a mode change, or a driver that
    // returned no metadata.
    bool full = false;
    // Desktop coordinates, not output-relative ones. Everything else in the
    // host speaks virtual-desktop pixels and this is no exception.
    std::vector<core::Recti> dirty;
    std::vector<MoveRect> moves;
    // Frames the driver coalesced into this one. Above 1 means the capture is
    // not keeping up with the screen, which is a fact worth surfacing rather
    // than one to hide.
    std::uint32_t accumulated = 0;
};

// One tile to encode straight out of the captured frame, in virtual-desktop
// pixels like everything else this interface speaks.
struct TileJob {
    core::Recti src{};
    // Encoded size, both multiples of four. Smaller than `src` is a box
    // average; equal is a straight copy.
    int out_w = 0;
    int out_h = 0;
};

class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    // The desktop area this source covers, in virtual-desktop pixels.
    virtual core::Recti bounds() const = 0;

    // Waits up to `timeout_ms` for the next frame. False means nothing new
    // arrived, which is the normal state of a still screen and not an error.
    virtual bool next(FrameUpdate& out, int timeout_ms) = 0;

    // Copies a desktop-coordinate rectangle out of the frame `next()` just
    // returned, as BGRA8 rows of `stride` bytes. Only valid until the next
    // call to next().
    //
    // A readback, and therefore the expensive path: it is here so the capture
    // can be looked at without a phone attached, and so a software encoder has
    // something to chew on. The real pipeline compresses on the GPU and never
    // brings the raw frame across.
    virtual bool read(core::Recti rect, std::vector<std::uint8_t>& out, int& stride) = 0;

    // Encodes tiles to ETC2 blocks without bringing the pixels across, when
    // the backend can do it. Blocks come back concatenated in job order,
    // exactly as the CPU path lays them out.
    //
    // Returning false is not a failure. It means this backend has no such
    // path, or cannot take this particular request, and the caller should read
    // pixels and encode them itself -- which is what every platform without a
    // shader for it does.
    virtual bool encode_tiles(std::span<const TileJob> /*jobs*/,
                              std::vector<std::uint8_t>& /*blocks*/) {
        return false;
    }

    // Microseconds the last encode_tiles spent queuing work and then waiting
    // for it. Only meaningful right after one that returned true.
    virtual double encode_dispatch_us() const { return 0.0; }
    virtual double encode_map_us() const { return 0.0; }
};

// Returns nullptr on platforms without an implementation yet.
std::unique_ptr<IFrameSource> make_frame_source();

} // namespace digitiz::host
