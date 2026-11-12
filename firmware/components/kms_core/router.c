#include "kms_core.h"
#include "kms_key_trace.h"
#include <string.h>

enum { TAP_NONE, TAP_DOWN, TAP_WAIT, TAP_SECOND };
static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
static bool keyboard_empty(const kms_state_t *s)
{
    if (s->modifiers || s->system) return false;
    for (unsigned i = 0; i < KMS_KEY_BYTES; ++i) if (s->keys[i]) return false;
    for (unsigned i = 0; i < KMS_CONSUMERS; ++i) if (s->consumer[i]) return false;
    return true;
}
bool kms_trigger_valid(unsigned trigger) { return trigger >= KMS_TRIGGER_CTRL && trigger <= KMS_TRIGGER_NUM_LOCK; }
const char *kms_trigger_name(kms_trigger_t trigger)
{
    switch (trigger) {
    case KMS_TRIGGER_CTRL: return "Ctrl";
    case KMS_TRIGGER_ALT: return "Alt";
    case KMS_TRIGGER_SHIFT: return "Shift";
    case KMS_TRIGGER_SCROLL_LOCK: return "Scroll Lock";
    case KMS_TRIGGER_INSERT: return "Insert";
    case KMS_TRIGGER_PAUSE: return "Pause/Break";
    case KMS_TRIGGER_NUM_LOCK: return "Num Lock";
    default: return "invalid";
    }
}
static unsigned trigger_key(kms_trigger_t trigger)
{
    switch (trigger) {
    case KMS_TRIGGER_SCROLL_LOCK: return KMS_HID_KEY_SCROLL_LOCK;
    case KMS_TRIGGER_INSERT: return KMS_HID_KEY_INSERT;
    case KMS_TRIGGER_PAUSE: return KMS_HID_KEY_PAUSE;
    case KMS_TRIGGER_NUM_LOCK: return KMS_HID_KEY_NUM_LOCK;
    default: return 0;
    }
}
static uint8_t trigger_mask(kms_trigger_t trigger)
{
    switch (trigger) {
    case KMS_TRIGGER_CTRL: return KMS_HID_CTRL_LEFT | KMS_HID_CTRL_RIGHT;
    case KMS_TRIGGER_ALT: return KMS_HID_ALT_LEFT | KMS_HID_ALT_RIGHT;
    case KMS_TRIGGER_SHIFT: return KMS_HID_SHIFT_LEFT | KMS_HID_SHIFT_RIGHT;
    default: return 0;
    }
}
static bool trigger_only(const kms_state_t *s, kms_trigger_t trigger)
{
    kms_state_t other = *s;
    uint8_t mask = trigger_mask(trigger);
    if (mask) {
        uint8_t m = s->modifiers;
        if (!m || (m & (m - 1)) || (m & mask) != m) return false;
        other.modifiers = 0;
    } else {
        unsigned key = trigger_key(trigger);
        if (!key || !kms_key(s, key)) return false;
        kms_set_key(&other, key, false);
    }
    return keyboard_empty(&other) && !s->buttons;
}
static void output(kms_router_t *r, int32_t x, int32_t y, int32_t wheel, int32_t pan)
{
    kms_state_t s = {0};
    for (unsigned i = 0; i < KMS_INPUTS; ++i) kms_state_merge(&s, &r->inputs[i].visible);
    kms_key_trace_emit(r->trace, r->ctx, KMS_KEY_ROUTED, KMS_KEY_OK,
        0, 0, 0, 0, 0, &r->trace_output, &s);
    r->trace_output = s;
    if (r->emit) r->emit(r->ctx, &s, x, y, wheel, pan);
}
static void render(kms_router_t *r, kms_input_t *i)
{
    for (unsigned b = 0; b < KMS_KEY_BYTES; ++b) {
        i->blocked.keys[b] &= i->physical.keys[b];
        i->visible.keys[b] = i->physical.keys[b] & (uint8_t)~i->blocked.keys[b];
    }
    i->blocked.modifiers &= i->physical.modifiers;
    i->blocked.buttons &= i->physical.buttons;
    i->blocked.system &= i->physical.system;
    i->visible.modifiers = i->physical.modifiers & (uint8_t)~i->blocked.modifiers;
    i->visible.buttons = i->physical.buttons & (uint16_t)~i->blocked.buttons;
    i->visible.system = i->physical.system & (uint8_t)~i->blocked.system;
    memset(i->visible.consumer, 0, sizeof(i->visible.consumer));
    for (unsigned c = 0; c < KMS_CONSUMERS; ++c) {
        bool blocked = false;
        for (unsigned b = 0; b < KMS_CONSUMERS; ++b)
            if (i->physical.consumer[c] && i->physical.consumer[c] == i->blocked.consumer[b]) blocked = true;
        if (!blocked) i->visible.consumer[c] = i->physical.consumer[c];
    }
    for (unsigned b = 0; b < KMS_CONSUMERS; ++b) {
        bool held = false;
        for (unsigned c = 0; c < KMS_CONSUMERS; ++c)
            if (i->blocked.consumer[b] == i->physical.consumer[c]) held = true;
        if (!held) i->blocked.consumer[b] = 0;
    }
    if (i->tap_phase == TAP_DOWN || i->tap_phase == TAP_SECOND) {
        i->visible.modifiers &= (uint8_t)~i->tap_modifier;
        if (!i->tap_modifier) kms_set_key(&i->visible, trigger_key(r->trigger), false);
    }
}
static void taps(kms_router_t *r, unsigned owner, uint8_t modifier, unsigned count)
{
    kms_input_t *i = &r->inputs[owner];
    if (i->tap_replay_blocked) return;
    kms_state_t original = i->visible;
    for (unsigned n = 0; n < count; ++n) {
        if (modifier) i->visible.modifiers |= modifier;
        else kms_set_key(&i->visible, trigger_key(r->trigger), true);
        output(r, 0, 0, 0, 0); i->visible = original; output(r, 0, 0, 0, 0);
    }
}
static void flush_tap(kms_router_t *r, unsigned owner)
{
    kms_input_t *i = &r->inputs[owner];
    unsigned phase = i->tap_phase; i->tap_phase = TAP_NONE;
    if (phase == TAP_WAIT || phase == TAP_SECOND) taps(r, owner, i->tap_modifier, 1);
    render(r, i);
    if (phase == TAP_DOWN || phase == TAP_SECOND) output(r, 0, 0, 0, 0);
}
void kms_router_init(kms_router_t *r, kms_emit_fn emit, kms_select_fn select, kms_pair_fn pair, void *ctx)
{
    memset(r, 0, sizeof(*r)); r->emit = emit; r->select = select; r->pair = pair; r->ctx = ctx;
    r->trigger = KMS_TRIGGER_DEFAULT; r->double_ms = KMS_DOUBLE_MS; r->select_ms = KMS_SELECT_MS;
}
bool kms_router_set_trigger(kms_router_t *r, kms_trigger_t trigger)
{
    if (!kms_trigger_valid(trigger)) return false;
    kms_router_block_held(r); r->trigger = trigger; return true;
}
bool kms_router_set_preferences(kms_router_t *r, const kms_user_preferences_t *p)
{
    if (!kms_user_preferences_valid(p)) return false;
    kms_router_set_trigger(r, (kms_trigger_t)p->trigger);
    r->double_ms = p->double_ms; r->select_ms = p->select_ms; return true;
}

enum kms_notice kms_router_take_notice(kms_router_t *r)
{ enum kms_notice n = r->notice; r->notice = KMS_NOTICE_NONE; return n; }
void kms_router_tick(kms_router_t *r, uint32_t now)
{
    for (unsigned n = 0; n < KMS_INPUTS; ++n)
        if (r->inputs[n].tap_phase && due(now, r->inputs[n].deadline)) flush_tap(r, n);
    if (r->mode && due(now, r->deadline)) {
        uint8_t mode = r->mode; r->mode = KMS_MODE_NORMAL; r->candidate = 0;
        if (mode == KMS_MODE_SELECT) taps(r, r->owner, r->tap_modifier, 2);
        if (mode == KMS_MODE_CONFIG || mode == KMS_MODE_DEFAULT) r->notice = KMS_NOTICE_CANCELLED;
    }
}
static void trace_pending_taps(kms_router_t *r, unsigned owner, enum kms_key_trace_reason reason)
{
    const kms_input_t *input = &r->inputs[owner];
    if (!r->trace || input->tap_replay_blocked) return;
    unsigned transitions = input->tap_phase == TAP_DOWN ? 1u : input->tap_phase == TAP_WAIT ? 2u :
        input->tap_phase == TAP_SECOND ? 3u : r->mode == KMS_MODE_SELECT && r->owner == owner ? 4u : 0u;
    kms_state_t zero = {0}, trigger = {0};
    trigger.modifiers = input->tap_phase ? input->tap_modifier : r->tap_modifier;
    if (!trigger.modifiers) kms_set_key(&trigger, trigger_key(r->trigger), true);
    for (unsigned n = 0; n < transitions; ++n)
        kms_key_trace_emit(r->trace, r->ctx, KMS_KEY_MISSING, reason, owner + 1u,
            0, 0, 0, 0, n % 2 ? &trigger : &zero, n % 2 ? &zero : &trigger);
}
void kms_router_block_output(kms_router_t *r)
{
    for (unsigned n = 0; n < KMS_INPUTS; ++n) {
        trace_pending_taps(r, n, KMS_KEY_HELD_BLOCKED);
        r->inputs[n].blocked = r->inputs[n].physical;
        memset(&r->inputs[n].visible, 0, sizeof(kms_state_t));
        // A retained prefix may still select a PC, but its fallback taps must
        // not replay discarded input if the command expires or is cancelled.
        if (r->inputs[n].tap_phase || (r->mode && r->owner == n)) r->inputs[n].tap_replay_blocked = true;
    }
}
void kms_router_block_held(kms_router_t *r)
{
    kms_router_block_output(r);
    for (unsigned n = 0; n < KMS_INPUTS; ++n) {
        r->inputs[n].tap_phase = TAP_NONE; r->inputs[n].tap_replay_blocked = false;
    }
    r->mode = KMS_MODE_NORMAL; r->candidate = 0;
}
void kms_router_remove(kms_router_t *r, unsigned input)
{
    if (input >= KMS_INPUTS) return;
    trace_pending_taps(r, input, KMS_KEY_SOURCE_REMOVED);
    memset(&r->inputs[input], 0, sizeof(r->inputs[input]));
    if (r->mode && r->owner == input) {
        if (r->mode == KMS_MODE_CONFIG || r->mode == KMS_MODE_DEFAULT) r->notice = KMS_NOTICE_CANCELLED;
        r->mode = KMS_MODE_NORMAL; r->candidate = 0;
    }
    output(r, 0, 0, 0, 0);
}
static bool others_clear(const kms_router_t *r, unsigned except)
{
    for (unsigned n = 0; n < KMS_INPUTS; ++n)
        if (n != except && (!keyboard_empty(&r->inputs[n].physical) || r->inputs[n].physical.buttons)) return false;
    return true;
}
static unsigned selected_slot(unsigned key)
{
    if (key >= KMS_HID_KEY_1 && key < KMS_HID_KEY_1 + KMS_SLOTS) return key - KMS_HID_KEY_1 + 1;
    if (key >= KMS_HID_KEYPAD_1 && key < KMS_HID_KEYPAD_1 + KMS_SLOTS) return key - KMS_HID_KEYPAD_1 + 1;
    return 0;
}
static void capture_keyboard(kms_input_t *i)
{
    memcpy(i->blocked.keys, i->physical.keys, sizeof(i->blocked.keys));
    memcpy(i->blocked.consumer, i->physical.consumer, sizeof(i->blocked.consumer));
    i->blocked.modifiers = i->physical.modifiers; i->blocked.system = i->physical.system;
}
static void configure_input(kms_router_t *r, unsigned input, const kms_state_t *old)
{
    kms_input_t *i = &r->inputs[input]; capture_keyboard(i);
    if (input != r->owner) return;
    if (kms_key(&i->physical, KMS_HID_KEY_ESCAPE) && !kms_key(old, KMS_HID_KEY_ESCAPE)) {
        r->mode = KMS_MODE_NORMAL; r->candidate = 0; r->notice = KMS_NOTICE_CANCELLED; return;
    }
    if (r->config_wait_release) {
        if (keyboard_empty(&i->physical)) r->config_wait_release = false;
        return;
    }
    if (r->candidate) {
        if (keyboard_empty(&i->physical)) {
            kms_trigger_t candidate = r->candidate;
            r->mode = KMS_MODE_NORMAL; r->candidate = 0;
            bool saved = r->configure && r->configure(r->ctx, candidate);
            if (saved) {
                r->trigger = candidate;
                for (unsigned n = 0; n < KMS_INPUTS; ++n) r->inputs[n].tap_phase = TAP_NONE;
            }
            r->notice = saved ? KMS_NOTICE_SAVED : KMS_NOTICE_ERROR;
        } else if (!trigger_only(&i->physical, r->candidate)) {
            r->candidate = 0; r->config_wait_release = true;
        }
        return;
    }
    for (unsigned t = KMS_TRIGGER_CTRL; t <= KMS_TRIGGER_NUM_LOCK; ++t)
        if (trigger_only(&i->physical, (kms_trigger_t)t)) { r->candidate = (kms_trigger_t)t; return; }
    if (!keyboard_empty(&i->physical)) r->config_wait_release = true;
}
static bool selection_overlaps_second_tap(const kms_router_t *r, const kms_input_t *i, const kms_state_t *state)
{
    if (i->tap_phase != TAP_SECOND || state->buttons) return false;
    kms_state_t remaining = *state;
    remaining.modifiers &= (uint8_t)~i->tap_modifier;
    if (!i->tap_modifier) kms_set_key(&remaining, trigger_key(r->trigger), false);
    unsigned slot_key = 0;
    for (unsigned k = 4; k < 224; ++k) if (kms_key(&remaining, k)) {
        bool shortcut = (k == KMS_HID_KEY_J && r->toggle_jiggler) ||
                        (k == KMS_HID_KEY_K && r->cycle_jiggler_mode) || (k == KMS_HID_KEY_L && r->dump_logs);
        if (slot_key || (!selected_slot(k) && !shortcut) || kms_key(&i->physical, k)) return false;
        slot_key = k;
    }
    if (!slot_key) return false;
    kms_set_key(&remaining, slot_key, false);
    return keyboard_empty(&remaining);
}
static void consumed_taps(kms_router_t *r)
{
    kms_state_t zero = {0}, trigger = {0};
    trigger.modifiers = r->tap_modifier;
    if (!trigger.modifiers) kms_set_key(&trigger, trigger_key(r->trigger), true);
    for (unsigned n = 0; n < 2; ++n) {
        kms_key_trace_emit(r->trace, r->ctx, KMS_KEY_CONSUMED, KMS_KEY_SHORTCUT,
            r->owner + 1u, 0, 0, 0, 0, &zero, &trigger);
        kms_key_trace_emit(r->trace, r->ctx, KMS_KEY_CONSUMED, KMS_KEY_SHORTCUT,
            r->owner + 1u, 0, 0, 0, 0, &trigger, &zero);
    }
}
static void route_input(kms_router_t *r, unsigned input, const kms_sample_t *s, uint32_t now)
{
    if (input >= KMS_INPUTS) return;
    if (r->diagnostic_typing) {
        kms_input_t *input_state = &r->inputs[input];
        bool pressed = (s->state.modifiers & (uint8_t)~input_state->physical.modifiers) ||
                       (s->state.system & (uint8_t)~input_state->physical.system);
        for (unsigned b = 0; b < KMS_KEY_BYTES; ++b)
            pressed |= (s->state.keys[b] & (uint8_t)~input_state->physical.keys[b]) != 0;
        for (unsigned c = 0; c < KMS_CONSUMERS; ++c) {
            bool held = false;
            for (unsigned p = 0; p < KMS_CONSUMERS; ++p)
                held |= s->state.consumer[c] == input_state->physical.consumer[p];
            pressed |= s->state.consumer[c] && !held;
        }
        bool consumed = r->console_input && r->console_input(r->ctx, &input_state->physical, &s->state);
        input_state->physical = s->state;
        if (pressed && !consumed) {
            if (r->stop_logs) r->stop_logs(r->ctx);
            r->diagnostic_typing = false;
        }
        kms_router_block_held(r); return;
    }
    kms_router_tick(r, now);
    kms_input_t *i = &r->inputs[input]; kms_state_t old = i->physical;
    bool key_event = old.modifiers != s->state.modifiers || old.system != s->state.system ||
                     memcmp(old.keys, s->state.keys, KMS_KEY_BYTES) || memcmp(old.consumer, s->state.consumer, sizeof(old.consumer));
    bool mouse_chord = old.buttons != s->state.buttons || s->wheel || s->pan;
    bool trigger = trigger_only(&s->state, r->trigger);
    // Fast typing may put the digit before, or in the same report as, the
    // second trigger release. Consume that selection before flushing the tap.
    bool fast_selection = !mouse_chord && selection_overlaps_second_tap(r, i, &s->state);
    if (fast_selection) {
        i->tap_phase = TAP_NONE; r->mode = KMS_MODE_SELECT;
        r->owner = (uint8_t)input; r->tap_modifier = i->tap_modifier; r->deadline = now + r->select_ms;
        i->blocked.modifiers |= i->tap_modifier;
        if (!i->tap_modifier) kms_set_key(&i->blocked, trigger_key(r->trigger), true);
    }
    if (key_event || mouse_chord) {
        for (unsigned n = 0; n < KMS_INPUTS; ++n)
            if (n != input && r->inputs[n].tap_phase) flush_tap(r, n);
        // Flush a retained ordinary key BEFORE a following letter/button is exposed.
        bool same_down = trigger && s->state.modifiers == i->tap_modifier;
        bool released = keyboard_empty(&s->state) && !s->state.buttons && trigger_only(&old, r->trigger);
        if (i->tap_phase && (mouse_chord || (!same_down && !released))) flush_tap(r, input);
    }
    i->physical = s->state;
    if (r->mode == KMS_MODE_CONFIG) {
        if (key_event) configure_input(r, input, &old);
        render(r, i); output(r, s->x, s->y, s->wheel, s->pan); return;
    }
    unsigned new_key = 0, new_count = 0;
    for (unsigned k = 4; k < 224; ++k) if (kms_key(&s->state, k) && !kms_key(&old, k)) { new_key = k; ++new_count; }
    uint8_t modifiers = s->state.modifiers;
    if (fast_selection) modifiers &= (uint8_t)~i->tap_modifier;
    bool extra = modifiers || s->state.system;
    for (unsigned c = 0; c < KMS_CONSUMERS; ++c) extra |= s->state.consumer[c] != 0;
    if (r->mode && key_event && (new_count || extra)) {
        uint8_t mode = r->mode; unsigned slot = selected_slot(new_key);
        bool command = !extra && new_count == 1;
        if (command && (new_key == KMS_HID_KEY_ESCAPE || slot ||
            (mode == KMS_MODE_SELECT && (new_key == KMS_HID_KEY_P || new_key == KMS_HID_KEY_C || new_key == KMS_HID_KEY_D ||
            (new_key == KMS_HID_KEY_J && r->toggle_jiggler) ||
            (new_key == KMS_HID_KEY_K && r->cycle_jiggler_mode) ||
            (new_key == KMS_HID_KEY_L && r->dump_logs))))) {
            if (mode == KMS_MODE_SELECT) consumed_taps(r);
            kms_set_key(&i->blocked, new_key, true); r->mode = KMS_MODE_NORMAL;
            if (new_key == KMS_HID_KEY_ESCAPE && mode == KMS_MODE_DEFAULT) r->notice = KMS_NOTICE_CANCELLED;
            if (new_key == KMS_HID_KEY_L) {
                r->diagnostic_typing = r->dump_logs(r->ctx);
                if (!r->diagnostic_typing) r->notice = KMS_NOTICE_ERROR;
            } else if (new_key == KMS_HID_KEY_J) {
                r->notice = r->toggle_jiggler(r->ctx) ? KMS_NOTICE_SAVED : KMS_NOTICE_ERROR;
            } else if (new_key == KMS_HID_KEY_K) {
                r->notice = r->cycle_jiggler_mode(r->ctx) ? KMS_NOTICE_SAVED : KMS_NOTICE_ERROR;
            } else if (new_key == KMS_HID_KEY_D) {
                r->mode = KMS_MODE_DEFAULT; r->deadline = now + r->select_ms;
            } else if (slot && mode == KMS_MODE_DEFAULT) {
                bool saved = r->save_boot_slot && r->save_boot_slot(r->ctx, slot);
                r->notice = saved ? KMS_NOTICE_SAVED : KMS_NOTICE_ERROR;
            } else if (new_key == KMS_HID_KEY_P) { r->mode = KMS_MODE_PAIR; r->deadline = now + r->select_ms; }
            else if (new_key == KMS_HID_KEY_C) {
                r->mode = KMS_MODE_CONFIG; r->owner = (uint8_t)input; r->candidate = 0;
                r->config_wait_release = true; r->deadline = now + KMS_CONFIG_MS;
            } else if (slot && mode == KMS_MODE_PAIR) { if (r->pair) r->pair(r->ctx, slot); }
            else if (slot && r->select && r->select(r->ctx, slot)) kms_router_block_held(r);
        } else {
            r->mode = KMS_MODE_NORMAL;
            if (mode == KMS_MODE_SELECT) taps(r, r->owner, r->tap_modifier, 2);
            if (mode == KMS_MODE_DEFAULT) r->notice = KMS_NOTICE_CANCELLED;
        }
    }
    if (!r->mode && key_event) {
        if (i->tap_phase == TAP_WAIT && trigger && s->state.modifiers == i->tap_modifier && keyboard_empty(&old)) {
            i->tap_phase = TAP_SECOND; i->deadline = now + r->double_ms;
        } else if ((i->tap_phase == TAP_DOWN || i->tap_phase == TAP_SECOND) &&
                   keyboard_empty(&s->state) && !s->state.buttons && trigger_only(&old, r->trigger)) {
            if (i->tap_phase == TAP_SECOND) {
                i->tap_phase = TAP_NONE; r->mode = KMS_MODE_SELECT;
                r->owner = (uint8_t)input; r->tap_modifier = i->tap_modifier; r->deadline = now + r->select_ms;
            } else { i->tap_phase = TAP_WAIT; i->deadline = now + r->double_ms; }
        } else if (i->tap_phase) {
            if (!(trigger && s->state.modifiers == i->tap_modifier)) flush_tap(r, input);
        } else if (trigger && keyboard_empty(&old) && others_clear(r, input)) {
            i->tap_replay_blocked = false;
            i->tap_modifier = s->state.modifiers; i->tap_phase = TAP_DOWN; i->deadline = now + r->double_ms;
        }
    }
    render(r, i); output(r, s->x, s->y, s->wheel, s->pan);
}

typedef struct {
    kms_router_t *router;
    kms_state_t before_output;
    kms_state_t before_visible;
    kms_state_t before_blocked;
    unsigned input;
    enum kms_key_trace_reason reason;
} filtered_trace_t;
static bool key_held(const kms_state_t *s, unsigned page, unsigned usage)
{
    if (page == 7) return usage >= 0xe0 && usage <= 0xe7 ?
        (s->modifiers & (1u << (usage - 0xe0))) != 0 : kms_key(s, usage);
    if (page == 1) return (s->system & (1u << (usage - 0x81))) != 0;
    for (unsigned n = 0; n < KMS_CONSUMERS; ++n) if (s->consumer[n] == usage) return true;
    return false;
}
static void filtered_key(void *ctx, unsigned page, unsigned usage, bool down)
{
    filtered_trace_t *filter = ctx;
    kms_router_t *r = filter->router;
    // The merged route already records transitions exposed to delivery. Two
    // keyboards holding the same key intentionally share one destination key.
    bool was = key_held(&filter->before_output, page, usage);
    bool is = key_held(&r->trace_output, page, usage);
    if (was != is && is == down) return;
    enum kms_key_trace_reason reason = filter->reason;
    bool was_visible = key_held(&filter->before_visible, page, usage);
    bool is_visible = key_held(&r->inputs[filter->input].visible, page, usage);
    if (was_visible != is_visible && is_visible == down && reason == KMS_KEY_OK) reason = KMS_KEY_MERGED;
    else if (reason == KMS_KEY_OK && key_held(down ? &r->inputs[filter->input].blocked : &filter->before_blocked, page, usage))
        reason = KMS_KEY_HELD_BLOCKED;
    if (!down && (reason == KMS_KEY_OK || reason == KMS_KEY_HELD_BLOCKED) && r->inputs[filter->input].trace_consumed_reason)
        reason = (enum kms_key_trace_reason)r->inputs[filter->input].trace_consumed_reason;
    enum kms_key_trace_stage stage = KMS_KEY_CONSUMED;
    if (reason == KMS_KEY_OK) { stage = KMS_KEY_MISSING; reason = KMS_KEY_ROUTER_DROPPED; }
    bool trigger = page == 7 && (trigger_mask(r->trigger) ?
        (usage >= 0xe0 && usage <= 0xe7 && (trigger_mask(r->trigger) & (1u << (usage - 0xe0)))) :
        usage == trigger_key(r->trigger));
    if (trigger && (r->inputs[filter->input].tap_phase || r->mode == KMS_MODE_SELECT)) stage = KMS_KEY_DEFERRED;
    if (down && stage == KMS_KEY_CONSUMED && reason != KMS_KEY_MERGED)
        r->inputs[filter->input].trace_consumed_reason = (uint8_t)reason;
    kms_state_t zero = {0}, key = {0};
    if (page == 7 && usage >= 0xe0 && usage <= 0xe7) key.modifiers = (uint8_t)(1u << (usage - 0xe0));
    else if (page == 7) kms_set_key(&key, usage, true);
    else if (page == 1) key.system = (uint8_t)(1u << (usage - 0x81));
    else key.consumer[0] = (uint16_t)usage;
    kms_key_trace_emit(r->trace, r->ctx, stage, reason, filter->input + 1u,
        0, 0, 0, 0, down ? &zero : &key, down ? &key : &zero);
}
void kms_router_input(kms_router_t *r, unsigned input, const kms_sample_t *s, uint32_t now)
{
    if (input >= KMS_INPUTS) return;
    kms_state_t previous = r->inputs[input].physical;
    filtered_trace_t filter = {.router = r, .before_output = r->trace_output,
        .before_visible = r->inputs[input].visible, .before_blocked = r->inputs[input].blocked, .input = input,
        .reason = r->diagnostic_typing ? KMS_KEY_CONSOLE : r->mode == KMS_MODE_CONFIG ?
            KMS_KEY_CONFIGURATION : (r->mode || r->inputs[input].tap_phase) ? KMS_KEY_SHORTCUT : KMS_KEY_OK};
    kms_key_trace_emit(r->trace, r->ctx, KMS_KEY_OBSERVED, KMS_KEY_OK,
        input + 1u, 0, 0, 0, 0, &previous, &s->state);
    route_input(r, input, s, now);
    if (filter.reason == KMS_KEY_OK && (r->mode || r->inputs[input].tap_phase)) filter.reason = KMS_KEY_SHORTCUT;
    if (r->trace) kms_key_changes(&previous, &s->state, filtered_key, &filter);
    if (keyboard_empty(&s->state)) r->inputs[input].trace_consumed_reason = 0;
}
