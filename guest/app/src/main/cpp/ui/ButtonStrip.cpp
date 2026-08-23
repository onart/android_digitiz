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
// stuck. A region button cannot be held down on the PC for longer than this —
// the menu opens instead — which is the price of putting edit and delete
// behind a hold rather than spending screen on a control per button.
constexpr double kLongPressSeconds = 0.55;
constexpr double kMoveSlopDp = 10.0;

float distance(core::Vec2 a, core::Vec2 b) {
    const core::Vec2 d = a - b;
    return static_cast<float>(std::sqrt(d.x * d.x + d.y * d.y));
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

    if (expanded_) {
        const int n = count();
        const auto fit = [&](float reserved) {
            const float room = available - m.toggle - m.add - reserved;
            return room <= 0.0f ? 0 : static_cast<int>(room / m.slot);
        };

        m.visible = fit(0.0f);
        if (m.visible < n) {
            // Not everything fits, so the arrows have to be paid for out of
            // the same run.
            m.arrows = true;
            m.visible = fit(m.arrow * 2.0f);
        }
        m.visible = std::clamp(m.visible, 0, n);
        if (m.visible >= n) {
            m.arrows = false;
        }
    }

    m.start = m.margin;
    m.cross = horizontal() ? static_cast<float>(surface_h) - m.margin - m.thickness : m.margin;
    m_ = m;

    // A shorter list, or a rotation, can leave the window past the end.
    if (count() == 0) {
        first_ = 0;
    } else {
        first_ = ((first_ % count()) + count()) % count();
    }
}

void ButtonStrip::advance(double dt_seconds) {
    if (press_ != Press::Slot || press_moved_ || region_dragging_) {
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

Rect ButtonStrip::slot_rect(int slot) const {
    const float offset =
        m_.toggle + (m_.arrows ? m_.arrow : 0.0f) + static_cast<float>(slot) * m_.slot;
    return cell(offset, m_.slot);
}

Rect ButtonStrip::next_rect() const {
    const float offset = m_.toggle + m_.arrow + static_cast<float>(m_.visible) * m_.slot;
    return cell(offset, m_.arrow);
}

Rect ButtonStrip::add_rect() const {
    const float offset = m_.toggle + (m_.arrows ? m_.arrow * 2.0f : 0.0f) +
                         static_cast<float>(m_.visible) * m_.slot;
    return cell(offset, m_.add);
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

    // Letterboxed, not stretched. The pad is a scale model of a PC rectangle,
    // and a model with the wrong proportions aims wrong: the middle of a wide
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

int ButtonStrip::button_at(int slot) const {
    const int n = count();
    if (n == 0 || slot < 0 || slot >= m_.visible) {
        return -1;
    }
    return (first_ + slot) % n;
}

const CustomButton* ButtonStrip::pressed_button() const {
    if (buttons_ == nullptr || press_slot_ < 0 ||
        static_cast<std::size_t>(press_slot_) >= buttons_->size()) {
        return nullptr;
    }
    return &(*buttons_)[static_cast<std::size_t>(press_slot_)];
}

void ButtonStrip::cycle(int delta) {
    const int n = count();
    if (n == 0) {
        return;
    }
    first_ = ((first_ + delta) % n + n) % n;
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
    region_dragging_ = false;

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

    for (int slot = 0; slot < m_.visible; ++slot) {
        if (!slot_rect(slot).contains(p)) {
            continue;
        }
        press_ = Press::Slot;
        press_slot_ = button_at(slot);
        // An empty slot still swallows the touch: it is part of the strip, and
        // drawing through a gap in it would be a surprise.
        return true;
    }

    // Anywhere else inside the strip is inert but still consumed.
    return occupied().contains(p);
}

void ButtonStrip::drag(core::Vec2 p) {
    if (press_ != Press::Slot) {
        return;
    }
    if (distance(p, press_pos_) > kMoveSlopDp * density_) {
        press_moved_ = true;
    }

    const CustomButton* button = pressed_button();
    if (button == nullptr || button->kind != ButtonKind::Region || !press_moved_) {
        return;
    }

    // Find the slot this button is sitting in, to rebuild its pad.
    for (int slot = 0; slot < m_.visible; ++slot) {
        if (button_at(slot) != press_slot_) {
            continue;
        }
        const Rect pad = region_pad(slot_rect(slot), *button);
        if (!region_dragging_) {
            region_dragging_ = true;
            // Only now is the press known to be a drag, so the button goes
            // down where the finger first landed, not where it has got to.
            emit_region(proto::PointerAction::Down, map_into_region(*button, pad, press_pos_));
        }
        emit_region(proto::PointerAction::Move, map_into_region(*button, pad, p));
        return;
    }
}

void ButtonStrip::release(core::Vec2 p) {
    switch (press_) {
    case Press::Toggle:
        expanded_ = !expanded_;
        state_changed_ = true;
        layout(surface_w_, surface_h_, density_);
        break;

    case Press::Prev:
        cycle(-1);
        break;

    case Press::Next:
        cycle(1);
        break;

    case Press::Add:
        add_requested_ = true;
        break;

    case Press::Slot: {
        const CustomButton* button = pressed_button();
        if (button == nullptr) {
            break;
        }
        if (region_dragging_) {
            for (int slot = 0; slot < m_.visible; ++slot) {
                if (button_at(slot) != press_slot_) {
                    continue;
                }
                const Rect pad = region_pad(slot_rect(slot), *button);
                emit_region(proto::PointerAction::Up, map_into_region(*button, pad, p));
                break;
            }
            break;
        }
        // Moving off a plain button cancels it, the way a button anywhere else
        // does. Region buttons never arrive here having moved: movement turned
        // them into a drag.
        if (!press_moved_) {
            activate(*button, press_pos_);
        }
        break;
    }

    case Press::None:
        break;
    }

    press_ = Press::None;
    press_slot_ = -1;
    region_dragging_ = false;
}

void ButtonStrip::emit_region(proto::PointerAction action, core::Vec2 pc) {
    last_region_pc_ = pc;
    if (pointer_) {
        pointer_(action, pc);
    }
}

void ButtonStrip::cancel_press() {
    if (region_dragging_) {
        // The host has a mouse button down on the user's desktop because of
        // us. Whatever else is going wrong, it does not get to stay down.
        emit_region(proto::PointerAction::Cancel, last_region_pc_);
    }
    press_ = Press::None;
    press_slot_ = -1;
    press_moved_ = false;
    region_dragging_ = false;
}

void ButtonStrip::activate(const CustomButton& button, core::Vec2 at) {
    switch (button.kind) {
    case ButtonKind::Point: {
        if (!pointer_) {
            break;
        }
        const core::Vec2 pc{static_cast<double>(button.target.x),
                            static_cast<double>(button.target.y)};
        pointer_(proto::PointerAction::Down, pc);
        pointer_(proto::PointerAction::Up, pc);
        break;
    }

    case ButtonKind::Region: {
        // A tap on the model is a click at the matching spot, by the same
        // mapping the drag uses.
        for (int slot = 0; slot < m_.visible; ++slot) {
            if (button_at(slot) != press_slot_) {
                continue;
            }
            const Rect pad = region_pad(slot_rect(slot), button);
            const core::Vec2 pc = map_into_region(button, pad, at);
            emit_region(proto::PointerAction::Down, pc);
            emit_region(proto::PointerAction::Up, pc);
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

    for (int slot = 0; slot < m_.visible; ++slot) {
        draw_slot(ui, slot);
    }

    // Plus sign.
    const Rect add = add_rect();
    const float len = 18.0f * density_;
    const float thick = 3.0f * density_;
    const float cx = add.x + add.w * 0.5f;
    const float cy = add.y + add.h * 0.5f;
    ui.rounded_rect(Rect{cx - len * 0.5f, cy - thick * 0.5f, len, thick}, thick * 0.5f, kAccent);
    ui.rounded_rect(Rect{cx - thick * 0.5f, cy - len * 0.5f, thick, len}, thick * 0.5f, kAccent);
}

void ButtonStrip::draw_slot(UiRenderer& ui, int slot) const {
    const int index = button_at(slot);
    if (index < 0) {
        return;
    }

    const Rect rect = slot_rect(slot);
    const float inset = 4.0f * density_;
    const Rect body{rect.x + inset, rect.y + inset, rect.w - inset * 2.0f, rect.h - inset * 2.0f};

    const CustomButton& button = (*buttons_)[static_cast<std::size_t>(index)];
    const bool held = press_ == Press::Slot && press_slot_ == index;

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

    for (int slot = 0; slot < m_.visible; ++slot) {
        const int index = button_at(slot);
        if (index < 0) {
            continue;
        }
        const CustomButton& button = (*buttons_)[static_cast<std::size_t>(index)];
        if (button.label.empty()) {
            continue;
        }
        const Rect rect = slot_rect(slot);
        text.draw(button.label, rect.x + rect.w * 0.5f, rect.y + rect.h - 19.0f * density_,
                  12.0f * density_, Color{0.86f, 0.89f, 0.95f, 1.0f}, TextAlign::Center);
    }

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
