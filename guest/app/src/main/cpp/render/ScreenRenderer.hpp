#pragma once

// Shows the PC's screen.
//
// The tiles arrive as ETC2 blocks and go straight into a compressed texture:
// glCompressedTexSubImage2D, no decode step anywhere on this side. That is the
// whole reason the format was chosen -- the GPU is the decoder, and it does it
// while sampling, for free.
//
// The texture holds the encoded surface, whose edges correspond to the edges
// of the desktop region the host was asked for. Drawing it is therefore just
// that rectangle put through the view transform, the same one everything else
// on this screen is positioned by.

#include <cstdint>

#include <GLES3/gl3.h>

#include <digitiz/core/geometry.hpp>

#include "screen/FrameReceiver.hpp"

namespace digitiz::guest {

class ScreenRenderer {
public:
    bool init();
    void release();

    bool ready() const noexcept { return program_ != 0; }

    // Hands one batch to the GPU. Rebuilds the texture first if the encoded
    // size changed, which loses what was on it -- the host marks everything
    // dirty when it changes the geometry, so it comes back.
    bool upload(const FrameBatch& batch);

    // True once there is something worth drawing.
    bool has_image() const noexcept { return texture_ != 0 && painted_; }

    // The desktop region the current texture covers.
    core::Recti region() const noexcept { return region_; }

    void draw(const core::ViewTransform& view, int surface_w, int surface_h) const;

    // Forget the picture without dropping the GL objects. For the link going
    // away: what is on screen is no longer what the PC looks like.
    void clear() noexcept { painted_ = false; }

private:
    bool ensure_texture(int w, int h);

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint texture_ = 0;

    GLint u_viewport_ = -1;
    GLint u_rect_ = -1;
    GLint u_image_ = -1;

    int tex_w_ = 0;
    int tex_h_ = 0;
    core::Recti region_{};
    bool painted_ = false;
};

} // namespace digitiz::guest
