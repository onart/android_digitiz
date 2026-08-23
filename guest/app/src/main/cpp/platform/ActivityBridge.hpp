#pragma once

// The things the guest needs from its Java half that are not text.
//
// TextRenderer keeps its own JNI attachment because it needs it on every
// rasterize; this is the other direction — occasional calls, made from
// wherever, that do not justify holding state.

struct GameActivity;

namespace digitiz::guest {

// Turns the display the other way round and remembers the choice, so the next
// launch comes up the same way. Done through the activity rather than by
// rotating what we draw: that way the system bars, the display cutout and the
// touch mapping all move with it.
//
// Safe to call from the render thread — the activity marshals to the UI
// thread itself.
void flip_orientation(GameActivity* activity);

} // namespace digitiz::guest
