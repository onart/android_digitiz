#include "ui/ButtonStrip.hpp"

#include <algorithm>
#include <cmath>

namespace digitiz::guest {

namespace {

const Color kAccent{0.30f, 0.62f, 0.46f, 1.0f};
const Color kPanel{0.13f, 0.14f, 0.17f, 0.94f};
const Color kSlot{0.19f, 0.20f, 0.25f, 0.96f};
const Color kSlotPressed{0.26f, 0.44f, 0.36f, 0.98f};
const Color kGlyph{0.78f, 0.82f, 0.89f, 1.0f};

// Long enough not to fire while someone is deciding, short enough not to feel
// stuck.
constexpr double kLongPressSeconds = 0.55;
constexpr double kMoveSlopDp = 10.0;
// How quickly the list settles onto a slot boundary after a drag or an arrow.
constexpr double kSnapSeconds = 0.14;

float distance(core::Vec2 a, core::Vec2 b) {
    const core::Vec2 d = a - b;
    return static_cast<float>(std::sqrt(d.x * d.x + d.y * d.y));
}

// Positive remainder, which fmod is not for negative input.
double wrap(double value, double span) {
    if (span <= 0.0) {
        return 0.0;
    }
    const double r = std::fmod(value, span);
    return r < 0.0 ? r + span : r;
}

} // namespace

void ButtonStrip::set_active_window(std::string process) {
    active_process_ = std::move(process);
}

void ButtonStrip::layout(int surface_w, int surface_h, float density) {
    surface_w_ = surface_w;
    surface_h_ = surface_h;
    density_ = density > 0.0f ? density : 1.0f;

    Metrics m;
    m.margin = 8.0f * density_;
    m.thickness = 76.0f * density_;
    m.toggle = 26.0f * density_;
    m.arrow = 34.0f * density_;
    m.add = 48.0f * density_;
    m.slot = (horizontal() ? 84.0f : 68.0f) * density_;

    const float axis = static_cast<float>(horizontal() ? surface_w : surface_h);
    const float available = axis - m.margin * 2.0f;

    // Slots that fit in a given length, with room for the arrows kept back.
    // Counting them in even when they are not drawn keeps the strip from
    // changing length the moment a button is added past the limit.
    const auto fit_in = [&](float budget) {
        const float room = budget - m.toggle - m.add - m.arrow * 2.0f;
        return room <= 0.0f ? 0 : static_cast<int>(room / m.slot);
    };

    max_slots_ = std::max(fit_in(available), 1);
    const int wanted = slot_limit_ > 0 ? slot_limit_ : std::max(fit_in(axis * 0.5f), 1);
    effective_limit_ = std::clamp(wanted, 1, max_slots_);

    if (expanded_) {
        m.visible = std::min(count(), effective_limit_);
        m.arrows = count() > m.visible;
        m.run = static_cast<float>(m.visible) * m.slot;
    }

    m_ = m;
    m_.start = m.margin;
    m_.cross = horizontal() ? static_cast<float>(surface_h) - m.margin - m.thickness : m.margin;

    if (!m_.arrows) {
        // Everything is on screen, so there is nowhere to be scrolled to.
        scroll_ = 0.0;
        scroll_target_ = 0.0;
    }
}

void ButtonStrip::advance(double dt_seconds) {
    if (!scrolling_ && scroll_ != scroll_target_) {
        const double t = std::clamp(dt_seconds / kSnapSeconds, 0.0, 1.0);
        scroll_ += (scroll_target_ - scroll_) * t;
        if (std::abs(scroll_target_ - scroll_) < 0.5) {
            scroll_ = scroll_target_;
        }
    }

    if (press_ != Press::Slot || press_moved_) {
        return;
    }
    press_seconds_ += dt_seconds;
    if (press_seconds_ < kLongPressSeconds) {
        return;
    }
    long_press_ = press_slot_;
    // Consumed here, so the release does not also fire the button.
    press_ = Press::None;
    press_slot_ = -1;
}

// --- geometry --------------------------------------------------------------

Rect ButtonStrip::cell(float offset, float length) const {
    if (horizontal()) {
        return Rect{m_.start + offset, m_.cross, length, m_.thickness};
    }
    return Rect{m_.cross, m_.start + offset, m_.thickness, length};
}

Rect ButtonStrip::toggle_rect() const {
    return cell(0.0f, m_.toggle);
}

Rect ButtonStrip::prev_rect() const {
    return cell(m_.toggle, m_.arrow);
}

float ButtonStrip::run_start() const {
    return m_.toggle + (m_.arrows ? m_.arrow : 0.0f);
}

Rect ButtonStrip::run_rect() const {
    return cell(run_start(), m_.run);
}

Rect ButtonStrip::slot_rect(int k) const {
    const float phase =
        scrollable() ? static_cast<float>(std::fmod(wrapped_scroll(), m_.slot)) : 0.0f;
    return cell(run_start() + static_cast<float>(k) * m_.slot - phase, m_.slot);
}

Rect ButtonStrip::next_rect() const {
    return cell(run_start() + m_.run, m_.arrow);
}

Rect ButtonStrip::add_rect() const {
    return cell(run_start() + m_.run + (m_.arrows ? m_.arrow : 0.0f), m_.add);
}

Rect ButtonStrip::occupied() const {
    if (!expanded_) {
        return toggle_rect();
    }
    const Rect add = add_rect();
    if (horizontal()) {
        return Rect{m_.start, m_.cross, add.x + add.w - m_.start, m_.thickness};
    }
    return Rect{m_.cross, m_.start, m_.thickness, add.y + add.h - m_.start};
}

core::Vec2 ButtonStrip::caption_origin() const {
    const Rect strip = occupied();
    if (horizontal()) {
        // Above the row: below it is the screen edge.
        return core::Vec2{strip.x, strip.y - 17.0f * density_};
    }
    // Below the column, running across rather than down a 76dp width.
    return core::Vec2{strip.x, strip.y + strip.h + 8.0f * density_};
}

Rect ButtonStrip::region_pad(const Rect& slot, const CustomButton& button) const {
    const float pad = 6.0f * density_;
    const float label = 15.0f * density_;
    const Rect box{slot.x + pad, slot.y + pad, slot.w - pad * 2.0f, slot.h - pad * 2.0f - label};
    if (box.w <= 0.0f || box.h <= 0.0f || button.target.w <= 0 || button.target.h <= 0) {
        return box;
    }

    // Letterboxed, not stretched. The pad is a scale model of a rectangle, and
    // a model with the wrong proportions aims wrong: the middle of a wide
    // toolbar is not the middle of a square.
    const float aspect = static_cast<float>(button.target.w) / static_cast<float>(button.target.h);
    float w = box.w;
    float h = w / aspect;
    if (h > box.h) {
        h = box.h;
        w = h * aspect;
    }
    return Rect{box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, w, h};
}

double ButtonStrip::along(core::Vec2 a, core::Vec2 b) const {
    return horizontal() ? a.x - b.x : a.y - b.y;
}

float ButtonStrip::wrapped_scroll() const {
    return static_cast<float>(wrap(scroll_, content_length()));
}

int ButtonStrip::button_at(int k) const {
    const int n = count();
    if (n == 0 || k < 0) {
        return -1;
    }
    if (!scrollable()) {
        return k < m_.visible ? k : -1;
    }
    // One past the last full slot is the one sliding in at the edge; the
    // scissor decides how much of it is seen.
    if (k > m_.visible) {
        return -1;
    }
    const int base = static_cast<int>(wrapped_scroll() / m_.slot);
    return (base + k) % n;
}

const CustomButton* ButtonStrip::pressed_button() const {
    if (buttons_ == nullptr || press_slot_ < 0 ||
        static_cast<std::size_t>(press_slot_) >= buttons_->size()) {
        return nullptr;
    }
    return &(*buttons_)[static_cast<std::size_t>(press_slot_)];
}

void ButtonStrip::nudge(int slots) {
    if (!scrollable()) {
        return;
    }
    scroll_target_ =
        std::round(scroll_ / m_.slot) * m_.slot + static_cast<double>(slots) * m_.slot;
}

void ButtonStrip::snap_to_slot() {
    if (!scrollable()) {
        scroll_target_ = scroll_;
        return;
    }
    scroll_target_ = std::round(scroll_ / m_.slot) * m_.slot;
}

core::Vec2 ButtonStrip::map_into_region(const CustomButton& button, const Rect& pad,
                                        core::Vec2 p) const {
    if (pad.w <= 0.0f || pad.h <= 0.0f) {
        return core::Vec2{static_cast<double>(button.target.x),
                          static_cast<double>(button.target.y)};
    }
    // Clamped, so a finger sliding off the model still aims at the edge of the
    // region rather than somewhere outside it.
    const double tx = std::clamp((p.x - pad.x) / pad.w, 0.0, 1.0);
    const double ty = std::clamp((p.y - pad.y) / pad.h, 0.0, 1.0);
    return core::Vec2{button.target.x + tx * std::max(button.target.w - 1, 0),
                      button.target.y + ty * std::max(button.target.h - 1, 0)};
}

// --- input -----------------------------------------------------------------

bool ButtonStrip::hit_test(core::Vec2 p) {
    press_ = Press::None;
    press_slot_ = -1;
    press_pos_ = p;
    press_seconds_ = 0.0;
    press_moved_ = false;
    scrolling_ = false;
    scroll_at_press_ = scroll_;

    if (toggle_rect().contains(p)) {
        press_ = Press::Toggle;
        return true;
    }
    if (!expanded_) {
        return false;
    }

    if (m_.arrows && prev_rect().contains(p)) {
        press_ = Press::Prev;
        return true;
    }
    if (m_.arrows && next_rect().contains(p)) {
        press_ = Press::Next;
        return true;
    }
    if (add_rect().contains(p)) {
        press_ = Press::Add;
        return true;
    }

    if (run_rect().contains(p)) {
        press_ = Press::Slot;
        for (int k = 0; k <= m_.visible; ++k) {
            if (slot_rect(k).contains(p)) {
                press_slot_ = button_at(k);
                break;
            }
        }
        // An empty run still takes the touch and can still be dragged: it is
        // part of the strip, and a gap that draws through to the canvas would
        // be a surprise.
        return true;
    }

    // Anywhere else inside the strip is inert but still consumed.
    return occupied().contains(p);
}

void ButtonStrip::drag(core::Vec2 p) {
    if (press_ != Press::Slot) {
        return;
    }
    if (!press_moved_ && distance(p, press_pos_) > kMoveSlopDp * density_) {
        press_moved_ = true;
        // Only a drag along the strip is a scroll. Across it is a finger
        // sliding off, and cancelling the button without scrolling is the
        // honest reading of that.
        const double across = horizontal() ? p.y - press_pos_.y : p.x - press_pos_.x;
        scrolling_ = scrollable() && std::abs(along(p, press_pos_)) > std::abs(across);
    }
    if (!scrolling_) {
        return;
    }
    // Dragging towards the start of the strip brings later buttons in.
    scroll_ = scroll_at_press_ - along(p, press_pos_);
    scroll_target_ = scroll_;
}

void ButtonStrip::release(core::Vec2 p) {
    (void)p;
    switch (press_) {
    case Press::Toggle:
        expanded_ = !expanded_;
        state_changed_ = true;
        layout(surface_w_, surface_h_, density_);
        break;

    case Press::Prev:
        nudge(-1);
        break;

    case Press::Next:
        nudge(1);
        break;

    case Press::Add:
        add_requested_ = true;
        break;

    case Press::Slot: {
        if (scrolling_) {
            scrolling_ = false;
            snap_to_slot();
            break;
        }
        // Moving off a button cancels it, the way a button anywhere else does.
        const CustomButton* button = pressed_button();
        if (button != nullptr && !press_moved_) {
            activate(*button, press_pos_);
        }
        break;
    }

    case Press::None:
        break;
    }

    press_ = Press::None;
    press_slot_ = -1;
    scrolling_ = false;
}

void ButtonStrip::cancel_press() {
    if (scrolling_) {
        scrolling_ = false;
        snap_to_slot();
    }
    press_ = Press::None;
    press_slot_ = -1;
    press_moved_ = false;
}

void ButtonStrip::activate(const CustomButton& button, core::Vec2 at) {
    switch (button.kind) {
    case ButtonKind::Point: {
        if (!pointer_) {
            break;
        }
        const core::Vec2 target{static_cast<double>(button.target.x),
                                static_cast<double>(button.target.y)};
        pointer_(proto::PointerAction::Down, target);
        pointer_(proto::PointerAction::Up, target);
        break;
    }

    case ButtonKind::Region: {
        // Still works for buttons already on the device, but no longer offered
        // in the editor -- see ButtonKind::Region in ButtonStore.hpp. Tap only
        // now: a drag inside the strip scrolls the list.
        for (int k = 0; k <= m_.visible; ++k) {
            if (button_at(k) != press_slot_) {
                continue;
            }
            const Rect pad = region_pad(slot_rect(k), button);
            const core::Vec2 target = map_into_region(button, pad, at);
            if (pointer_) {
                pointer_(proto::PointerAction::Down, target);
                pointer_(proto::PointerAction::Up, target);
            }
            break;
        }
        break;
    }

    case ButtonKind::Shortcut:
        if (shortcut_) {
            shortcut_(button);
        }
        break;
    }
}

bool ButtonStrip::take_add_request() noexcept {
    const bool asked = add_requested_;
    add_requested_ = false;
    return asked;
}

bool ButtonStrip::take_state_change() noexcept {
    const bool changed = state_changed_;
    state_changed_ = false;
    return changed;
}

int ButtonStrip::take_long_press() noexcept {
    const int index = long_press_;
    long_press_ = -1;
    return index;
}

// --- drawing ---------------------------------------------------------------

void ButtonStrip::draw(UiRenderer& ui) const {
    const float radius = 12.0f * density_;
    ui.rounded_rect(occupied(), radius, kPanel);

    // The tab: a short bar along the strip direction, so it reads as a grip
    // rather than as another button.
    const Rect toggle = toggle_rect();
    const float bar = 3.0f * density_;
    const float span = 22.0f * density_;
    const Rect grip = horizontal() ? Rect{toggle.x + (toggle.w - bar) * 0.5f,
                                          toggle.y + (toggle.h - span) * 0.5f, bar, span}
                                   : Rect{toggle.x + (toggle.w - span) * 0.5f,
                                          toggle.y + (toggle.h - bar) * 0.5f, span, bar};
    ui.rounded_rect(grip, bar * 0.5f, Color{0.55f, 0.59f, 0.67f, 0.95f});

    if (!expanded_) {
        return;
    }

    if (m_.arrows) {
        const float dot = 6.0f * density_;
        const Rect arrows[2] = {prev_rect(), next_rect()};
        for (const Rect& r : arrows) {
            ui.rounded_rect(Rect{r.x + (r.w - dot) * 0.5f, r.y + (r.h - dot) * 0.5f, dot, dot},
                            dot * 0.5f, Color{0.70f, 0.74f, 0.82f, 0.95f});
        }
    }

    // The extra slot is the one sliding in at the edge; the clip cuts it off
    // at the end of the run rather than letting it cross the screen.
    ui.set_clip(run_rect());
    for (int k = 0; k <= m_.visible; ++k) {
        draw_slot(ui, k);
    }
    ui.clear_clip();

    // Plus sign.
    const Rect add = add_rect();
    const float len = 18.0f * density_;
    const float thick = 3.0f * density_;
    const float cx = add.x + add.w * 0.5f;
    const float cy = add.y + add.h * 0.5f;
    ui.rounded_rect(Rect{cx - len * 0.5f, cy - thick * 0.5f, len, thick}, thick * 0.5f, kAccent);
    ui.rounded_rect(Rect{cx - thick * 0.5f, cy - len * 0.5f, thick, len}, thick * 0.5f, kAccent);
}

void ButtonStrip::draw_slot(UiRenderer& ui, int k) const {
    const int index = button_at(k);
    if (index < 0) {
        return;
    }

    const Rect rect = slot_rect(k);
    const float inset = 4.0f * density_;
    const Rect body{rect.x + inset, rect.y + inset, rect.w - inset * 2.0f, rect.h - inset * 2.0f};

    const CustomButton& button = (*buttons_)[static_cast<std::size_t>(index)];
    const bool held = press_ == Press::Slot && press_slot_ == index && !press_moved_;

    ui.rounded_rect(body, 10.0f * density_, held ? kSlotPressed : kSlot);

    switch (button.kind) {
    case ButtonKind::Point: {
        // A crosshair: one place on the PC.
        const float cx = body.x + body.w * 0.5f;
        const float cy = body.y + body.h * 0.5f - 6.0f * density_;
        const float len = 16.0f * density_;
        const float bar = 2.0f * density_;
        ui.rounded_rect(Rect{cx - len * 0.5f, cy - bar * 0.5f, len, bar}, bar * 0.5f, kGlyph);
        ui.rounded_rect(Rect{cx - bar * 0.5f, cy - len * 0.5f, bar, len}, bar * 0.5f, kGlyph);
        const float dot = 6.0f * density_;
        ui.rounded_rect(Rect{cx - dot * 0.5f, cy - dot * 0.5f, dot, dot}, dot * 0.5f, kAccent);
        break;
    }

    case ButtonKind::Region: {
        const Rect pad = region_pad(rect, button);
        ui.rounded_rect(pad, 3.0f * density_, Color{0.10f, 0.11f, 0.14f, 1.0f});
        ui.rounded_rect_outline(pad, 3.0f * density_, 1.5f * density_,
                                Color{kAccent.r, kAccent.g, kAccent.b, 0.95f});
        break;
    }

    case ButtonKind::Shortcut: {
        // A keycap.
        const float w = 26.0f * density_;
        const float h = 20.0f * density_;
        const Rect cap{body.x + (body.w - w) * 0.5f,
                       body.y + body.h * 0.5f - 6.0f * density_ - h * 0.5f, w, h};
        ui.rounded_rect_outline(cap, 4.0f * density_, 1.5f * density_, kGlyph);
        break;
    }
    }
}

void ButtonStrip::load_labels(TextRenderer& text) {
    if (labels_loaded_) {
        return;
    }
    caption_label_ = text.localized("strip_active_window");
    caption_none_ = text.localized("strip_active_window_none");
    labels_loaded_ = true;
}

void ButtonStrip::draw_labels(TextRenderer& text) const {
    if (!expanded_) {
        return;
    }

    text.set_clip(run_rect());
    for (int k = 0; k <= m_.visible; ++k) {
        const int index = button_at(k);
        if (index < 0) {
            continue;
        }
        const CustomButton& button = (*buttons_)[static_cast<std::size_t>(index)];
        if (button.label.empty()) {
            continue;
        }
        const Rect rect = slot_rect(k);
        text.draw(button.label, rect.x + rect.w * 0.5f, rect.y + rect.h - 19.0f * density_,
                  12.0f * density_, Color{0.86f, 0.89f, 0.95f, 1.0f}, TextAlign::Center);
    }
    text.clear_clip();

    if (!labels_loaded_) {
        return;
    }
    const core::Vec2 origin = caption_origin();
    const bool known = !active_process_.empty();
    const float x = static_cast<float>(origin.x);
    const float y = static_cast<float>(origin.y);
    const float advance =
        text.draw(caption_label_, x, y, 11.0f * density_, Color{0.50f, 0.54f, 0.62f, 0.95f});
    text.draw(known ? active_process_ : caption_none_, x + advance + 6.0f * density_, y,
              11.0f * density_,
              known ? Color{0.72f, 0.78f, 0.86f, 0.95f} : Color{0.44f, 0.47f, 0.55f, 0.95f});
}

} // namespace digitiz::guest
