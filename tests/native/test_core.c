#include "kms_core.h"
#include <string.h>

#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static int checks;
static kms_router_t router;
static kms_state_t emitted[256];
static unsigned count, selected, paired;
static bool online = true;
static unsigned saves;
static kms_trigger_t saved;
static bool save_ok;
static void emit(void *ctx, const kms_state_t *s, int32_t x, int32_t y, int32_t w, int32_t p)
{ (void)ctx; (void)x; (void)y; (void)w; (void)p; if (count < 256) emitted[count++] = *s; }
static bool select_slot(void *ctx, unsigned s) { (void)ctx; selected = s; return online; }
static void pair_slot(void *ctx, unsigned s) { (void)ctx; paired = s; }
static bool save_boot(void *ctx, unsigned slot)
{ (void)ctx; ++saves; paired = slot; return save_ok; }
static bool configure(void *ctx, kms_trigger_t trigger)
{ (void)ctx; ++saves; saved = trigger; return save_ok; }
static void reset(void)
{
    kms_router_init(&router, emit, select_slot, pair_slot, 0); router.configure = configure; router.save_boot_slot = save_boot;
    count = selected = paired = saves = 0; saved = 0; online = save_ok = true;
}
static void key(unsigned input, uint8_t mods, unsigned k, uint32_t now)
{ kms_sample_t s = {0}; s.state.modifiers = mods; if (k) kms_set_key(&s.state, k, true); kms_router_input(&router, input, &s, now); }
static void double_ctrl(uint8_t ctrl)
{ key(0, ctrl, 0, 1); key(0, 0, 0, 50); key(0, ctrl, 0, 100); key(0, 0, 0, 150); }
static const uint8_t trigger_mods[] = {0, 1, 4, 2, 0, 0, 0, 0};
static const unsigned trigger_keys[] = {0, 0, 0, 0, KMS_HID_KEY_SCROLL_LOCK, KMS_HID_KEY_INSERT, KMS_HID_KEY_PAUSE, KMS_HID_KEY_NUM_LOCK};
static void double_trigger(kms_trigger_t trigger, uint32_t now)
{
    key(0, trigger_mods[trigger], trigger_keys[trigger], now);
    key(0, 0, 0, now + 40);
    key(0, trigger_mods[trigger], trigger_keys[trigger], now + 80);
    key(0, 0, 0, now + 120);
}
static void open_config(void)
{
    double_ctrl(1); key(0, 0, KMS_HID_KEY_C, 200); key(0, 0, 0, 220);
}
static bool all_neutral(void)
{
    kms_state_t zero = {0};
    for (unsigned n = 0; n < count; ++n) if (!kms_state_equal(&emitted[n], &zero)) return false;
    return true;
}
static enum kms_button_event button_click(kms_button_t *b, uint32_t now)
{
    (void)kms_button_update(b, true, now);
    (void)kms_button_update(b, true, now + KMS_BUTTON_DEBOUNCE_MS);
    (void)kms_button_update(b, false, now + 80);
    return kms_button_update(b, false, now + 80 + KMS_BUTTON_DEBOUNCE_MS);
}
static int controls_tests(void)
{
    CHECK(!kms_trigger_valid(0) && !kms_trigger_valid(KMS_TRIGGER_NUM_LOCK + 1));
    // A digit can overlap the second trigger or replace it in one HID report.
    for (unsigned t = KMS_TRIGGER_CTRL; t <= KMS_TRIGGER_NUM_LOCK; ++t)
        for (unsigned slot = 1; slot <= KMS_SLOTS; ++slot)
            for (unsigned overlap = 0; overlap < 2; ++overlap)
                for (unsigned keypad = 0; keypad < 2; ++keypad)
                    for (unsigned available = 0; available < 2; ++available) {
                        reset(); (void)kms_router_set_trigger(&router, (kms_trigger_t)t);
                        online = available != 0;
                        key(0, trigger_mods[t], trigger_keys[t], 1);
                        key(0, 0, 0, 2);
                        key(0, trigger_mods[t], trigger_keys[t], 3);
                        unsigned digit = (keypad ? KMS_HID_KEYPAD_1 : KMS_HID_KEY_1) + slot - 1;
                        kms_sample_t sample = {0}; kms_set_key(&sample.state, digit, true);
                        if (overlap) {
                            sample.state.modifiers = trigger_mods[t];
                            if (trigger_keys[t]) kms_set_key(&sample.state, trigger_keys[t], true);
                        }
                        kms_router_input(&router, 0, &sample, 4);
                        CHECK(selected == slot && router.mode == KMS_MODE_NORMAL && all_neutral());
                        // Keep holding the digit after releasing the trigger.
                        key(0, 0, digit, 5); key(0, 0, 0, 6);
                        CHECK(all_neutral());
                        key(0, 0, digit, 7);
                        CHECK(kms_key(&emitted[count - 1], digit));
                    }
    for (unsigned t = KMS_TRIGGER_CTRL; t <= KMS_TRIGGER_NUM_LOCK; ++t) {
        reset(); CHECK(kms_router_set_trigger(&router, (kms_trigger_t)t));
        double_trigger((kms_trigger_t)t, 1); CHECK(router.mode == KMS_MODE_SELECT);
        key(0, 0, KMS_HID_KEY_1, 200); CHECK(selected == 1 && all_neutral());
        key(0, 0, 0, 210); key(0, 0, KMS_HID_KEY_1, 220);
        CHECK(kms_key(&emitted[count - 1], KMS_HID_KEY_1));
        reset(); (void)kms_router_set_trigger(&router, (kms_trigger_t)t);
        double_trigger((kms_trigger_t)t, 1); count = 0;
        kms_router_tick(&router, router.deadline);
        CHECK(count == 4 && router.mode == KMS_MODE_NORMAL);
        CHECK(emitted[0].modifiers == trigger_mods[t]);
        CHECK(trigger_keys[t] ? kms_key(&emitted[0], trigger_keys[t]) : emitted[0].modifiers != 0);
        CHECK(kms_state_equal(&emitted[0], &emitted[2]) && kms_state_equal(&emitted[1], &emitted[3]));
        CHECK(!emitted[1].modifiers && !kms_key(&emitted[1], trigger_keys[t]));
        // Every choice can be saved, and nothing from configuration reaches the PC.
        reset(); open_config(); CHECK(router.mode == KMS_MODE_CONFIG && !router.config_wait_release);
        key(0, trigger_mods[t], trigger_keys[t], 250); CHECK(saves == 0);
        key(0, 0, 0, 280); CHECK(saves == 1 && saved == (kms_trigger_t)t && router.trigger == (kms_trigger_t)t);
        CHECK(kms_router_take_notice(&router) == KMS_NOTICE_SAVED && all_neutral());
        CHECK(kms_router_take_notice(&router) == KMS_NOTICE_NONE);
        double_trigger((kms_trigger_t)t, 400); key(0, 0, KMS_HID_KEY_1, 550); CHECK(selected == 1);
    }
    // One tap plus a digit remains ordinary typing, as does a second-tap letter chord.
    for (unsigned second = 0; second < 2; ++second) {
        reset(); (void)kms_router_set_trigger(&router, KMS_TRIGGER_INSERT);
        key(0, 0, KMS_HID_KEY_INSERT, 1);
        if (second) { key(0, 0, 0, 2); key(0, 0, KMS_HID_KEY_INSERT, 3); }
        unsigned following = second ? 4 : KMS_HID_KEY_1;
        kms_sample_t chord = {0};
        kms_set_key(&chord.state, KMS_HID_KEY_INSERT, true);
        kms_set_key(&chord.state, following, true);
        kms_router_input(&router, 0, &chord, 4);
        CHECK(!selected && router.mode == KMS_MODE_NORMAL);
        CHECK(kms_key(&emitted[count - 1], following) && kms_key(&emitted[count - 1], KMS_HID_KEY_INSERT));
    }
    // A double tap cannot mix the left and right modifier or different keyboards.
    for (unsigned t = KMS_TRIGGER_CTRL; t <= KMS_TRIGGER_SHIFT; ++t) {
        reset(); (void)kms_router_set_trigger(&router, (kms_trigger_t)t);
        key(0, trigger_mods[t], 0, 1); key(0, 0, 0, 30);
        key(0, (uint8_t)(trigger_mods[t] << 4), 0, 60); key(0, 0, 0, 90);
        CHECK(router.mode == KMS_MODE_NORMAL);
        reset(); (void)kms_router_set_trigger(&router, (kms_trigger_t)t);
        double_ctrl((uint8_t)(trigger_mods[t] << 4)); CHECK(router.mode == KMS_MODE_SELECT);
    }
    reset(); (void)kms_router_set_trigger(&router, KMS_TRIGGER_ALT);
    key(0, KMS_HID_CTRL_LEFT | KMS_HID_ALT_RIGHT, 4, 1);
    CHECK(emitted[count - 1].modifiers == (KMS_HID_CTRL_LEFT | KMS_HID_ALT_RIGHT));
    CHECK(kms_key(&emitted[count - 1], 4) && !router.mode); // AltGr chord.
    reset(); (void)kms_router_set_trigger(&router, KMS_TRIGGER_INSERT);
    key(0, 0, KMS_HID_KEY_INSERT, 1); count = 0; key(0, 0, 4, 20);
    CHECK(count >= 2 && kms_key(&emitted[0], KMS_HID_KEY_INSERT) && !kms_key(&emitted[0], 4));
    CHECK(kms_key(&emitted[count - 1], 4) && !kms_key(&emitted[count - 1], KMS_HID_KEY_INSERT));
    reset(); (void)kms_router_set_trigger(&router, KMS_TRIGGER_INSERT);
    key(0, 0, KMS_HID_KEY_INSERT, 1); kms_router_tick(&router, 1 + KMS_DOUBLE_MS);
    CHECK(kms_key(&emitted[count - 1], KMS_HID_KEY_INSERT));
    key(0, 0, 0, 400); CHECK(!kms_key(&emitted[count - 1], KMS_HID_KEY_INSERT));
    reset(); (void)kms_router_set_trigger(&router, KMS_TRIGGER_SCROLL_LOCK);
    double_trigger(KMS_TRIGGER_SCROLL_LOCK, 1); key(0, 0, KMS_HID_KEY_P, 200);
    key(0, 0, 0, 210); key(0, 0, KMS_HID_KEY_1, 220); CHECK(paired == 1 && all_neutral());
    reset(); open_config(); key(0, 0, KMS_HID_KEY_ESCAPE, 250);
    CHECK(router.trigger == KMS_TRIGGER_CTRL && !saves && !router.mode && all_neutral());
    CHECK(kms_router_take_notice(&router) == KMS_NOTICE_CANCELLED);
    reset(); (void)kms_router_set_trigger(&router, KMS_TRIGGER_SCROLL_LOCK);
    double_trigger(KMS_TRIGGER_SCROLL_LOCK, 1); key(0, 0, KMS_HID_KEY_C, 200); key(0, 0, 0, 220);
    key(0, 0, KMS_HID_KEY_ESCAPE, 250);
    CHECK(router.trigger == KMS_TRIGGER_SCROLL_LOCK && !saves && all_neutral());
    reset(); open_config(); key(0, 0, KMS_HID_KEY_INSERT, 250);
    kms_router_tick(&router, router.deadline); key(0, 0, KMS_HID_KEY_INSERT, 11000);
    CHECK(!saves && !router.mode && router.trigger == KMS_TRIGGER_CTRL && all_neutral());
    key(0, 0, 0, 11010); key(0, 0, KMS_HID_KEY_INSERT, 11020);
    CHECK(kms_key(&emitted[count - 1], KMS_HID_KEY_INSERT));
    reset(); open_config(); key(0, 0, 4, 250); key(0, 0, 0, 270);
    CHECK(router.mode == KMS_MODE_CONFIG && !saves && all_neutral());
    key(0, KMS_HID_SHIFT_LEFT, 0, 280); key(0, KMS_HID_SHIFT_LEFT, 4, 290); key(0, 0, 0, 300);
    CHECK(router.mode == KMS_MODE_CONFIG && !saves && all_neutral());
    key(1, 0, KMS_HID_KEY_SCROLL_LOCK, 310); key(1, 0, 0, 320);
    CHECK(!saves && router.mode == KMS_MODE_CONFIG && all_neutral());
    kms_sample_t motion = {.x = 3}; motion.state.buttons = 1;
    kms_router_input(&router, 2, &motion, 325);
    CHECK(emitted[count - 1].buttons == 1 && router.mode == KMS_MODE_CONFIG);
    motion.state.buttons = 0; kms_router_input(&router, 2, &motion, 326);
    key(0, 0, KMS_HID_KEY_NUM_LOCK, 330); key(0, 0, 0, 340);
    CHECK(saves == 1 && router.trigger == KMS_TRIGGER_NUM_LOCK && !emitted[count - 1].modifiers);
    reset(); open_config(); save_ok = false; key(0, 0, KMS_HID_KEY_INSERT, 250); key(0, 0, 0, 280);
    CHECK(saves == 1 && router.trigger == KMS_TRIGGER_CTRL && !router.mode && all_neutral());
    CHECK(kms_router_take_notice(&router) == KMS_NOTICE_ERROR);
    reset(); open_config(); kms_router_remove(&router, 0);
    CHECK(!saves && !router.mode && kms_router_take_notice(&router) == KMS_NOTICE_CANCELLED);
    reset(); key(0, 1, 0, 1); CHECK(!kms_router_set_trigger(&router, (kms_trigger_t)99));
    CHECK(router.trigger == KMS_TRIGGER_CTRL);
    CHECK(kms_router_set_trigger(&router, KMS_TRIGGER_INSERT));
    key(0, 1, 0, 10); CHECK(!emitted[count - 1].modifiers);
    key(0, 0, 0, 20); key(0, 1, 0, 30); CHECK(emitted[count - 1].modifiers == 1);

    for (unsigned slot = 1; slot <= KMS_SLOTS; ++slot) {
        reset(); kms_router_set_trigger(&router, KMS_TRIGGER_INSERT);
        double_trigger(KMS_TRIGGER_INSERT, 1); key(0, 0, KMS_HID_KEY_D, 200); key(0, 0, 0, 220);
        CHECK(router.mode == KMS_MODE_DEFAULT);
        key(0, 0, KMS_HID_KEY_1 + slot - 1, 240);
        CHECK(saves == 1 && paired == slot && selected == 0 && !router.mode && all_neutral());
        CHECK(kms_router_take_notice(&router) == KMS_NOTICE_SAVED);
    }
    reset(); double_ctrl(1); key(0, 0, KMS_HID_KEY_D, 200); key(0, 0, 0, 220);
    key(0, 0, KMS_HID_KEYPAD_1 + 1, 240); CHECK(paired == 2 && saves == 1 && all_neutral());
    reset(); double_ctrl(1); key(0, 0, KMS_HID_KEY_D, 200); key(0, 0, 0, 220);
    key(0, 0, KMS_HID_KEY_ESCAPE, 240); CHECK(!saves && !router.mode && all_neutral());
    CHECK(kms_router_take_notice(&router) == KMS_NOTICE_CANCELLED);
    reset(); double_ctrl(1); key(0, 0, KMS_HID_KEY_D, 200); key(0, 0, 0, 220);
    kms_router_tick(&router, router.deadline); CHECK(!saves && !router.mode && all_neutral());
    CHECK(kms_router_take_notice(&router) == KMS_NOTICE_CANCELLED);
    reset(); double_ctrl(1); key(0, 0, KMS_HID_KEY_D, 200); key(0, 0, 0, 220);
    save_ok = false; key(0, 0, KMS_HID_KEY_1, 240);
    CHECK(!selected && kms_router_take_notice(&router) == KMS_NOTICE_ERROR && all_neutral());
    kms_button_t b; const uint32_t normal = KMS_FACTORY_RESET_WINDOW_MS + 100;
    kms_button_init(&b, true, 0);
    CHECK(button_click(&b, normal) == KMS_BUTTON_NONE);
    CHECK(button_click(&b, normal + 200) == KMS_BUTTON_NONE);
    CHECK(button_click(&b, normal + 400) == KMS_BUTTON_RESTORE_TRIGGER);
    CHECK(kms_button_update(&b, false, normal + 600) == KMS_BUTTON_NONE);
    kms_button_init(&b, true, 0); (void)button_click(&b, normal);
    CHECK(button_click(&b, normal + KMS_BUTTON_MULTI_PRESS_MS + 200) == KMS_BUTTON_NONE);
    CHECK(button_click(&b, normal + KMS_BUTTON_MULTI_PRESS_MS + 400) == KMS_BUTTON_NONE);
    // Sub-debounce noise does not count as a press.
    kms_button_init(&b, true, 0); (void)kms_button_update(&b, true, normal);
    (void)kms_button_update(&b, false, normal + KMS_BUTTON_DEBOUNCE_MS - 1);
    CHECK(kms_button_update(&b, false, normal + 100) == KMS_BUTTON_NONE && !b.clicks && !b.stable);
    // Receiver pairing keeps the original long-press behavior, including at startup.
    kms_button_init(&b, false, 0); (void)kms_button_update(&b, true, 1);
    uint32_t pressed = 1 + KMS_BUTTON_DEBOUNCE_MS;
    (void)kms_button_update(&b, true, pressed);
    CHECK(kms_button_update(&b, true, pressed + KMS_BUTTON_HOLD_MS - 1) == KMS_BUTTON_NONE);
    CHECK(kms_button_update(&b, true, pressed + KMS_BUTTON_HOLD_MS) == KMS_BUTTON_LONG);
    CHECK(kms_button_update(&b, true, pressed + KMS_BUTTON_HOLD_MS + 1) == KMS_BUTTON_NONE && !b.factory_ready);
    // Factory reset requires a startup hold AND a subsequent debounced release.
    kms_button_init(&b, true, 0); (void)kms_button_update(&b, true, 1);
    (void)kms_button_update(&b, true, pressed);
    CHECK(kms_button_update(&b, true, pressed + KMS_BUTTON_HOLD_MS) == KMS_BUTTON_NONE);
    CHECK(kms_button_update(&b, true, pressed + KMS_FACTORY_RESET_HOLD_MS - 1) == KMS_BUTTON_NONE && !b.factory_ready);
    CHECK(kms_button_update(&b, true, pressed + KMS_FACTORY_RESET_HOLD_MS) == KMS_BUTTON_NONE && b.factory_ready);
    uint32_t release = pressed + KMS_FACTORY_RESET_HOLD_MS + 100;
    CHECK(kms_button_update(&b, false, release) == KMS_BUTTON_NONE);
    CHECK(kms_button_update(&b, false, release + KMS_BUTTON_DEBOUNCE_MS) == KMS_BUTTON_FACTORY_RESET);
    CHECK(kms_button_update(&b, false, release + 100) == KMS_BUTTON_NONE);
    kms_button_init(&b, true, 0); (void)kms_button_update(&b, true, 1);
    (void)kms_button_update(&b, true, pressed);
    (void)kms_button_update(&b, false, pressed + KMS_FACTORY_RESET_HOLD_MS - 100);
    CHECK(kms_button_update(&b, false, pressed + KMS_FACTORY_RESET_HOLD_MS) == KMS_BUTTON_NONE && !b.factory_ready && !b.clicks);
    kms_button_init(&b, true, 0); (void)kms_button_update(&b, true, normal);
    (void)kms_button_update(&b, true, normal + KMS_BUTTON_DEBOUNCE_MS);
    CHECK(kms_button_update(&b, true, normal + KMS_BUTTON_DEBOUNCE_MS + KMS_BUTTON_HOLD_MS) == KMS_BUTTON_LONG && !b.factory_armed);
    // Clock wrap must not turn a normal press into factory reset.
    kms_button_init(&b, true, 0xfffffff0u);
    CHECK(button_click(&b, 0x20u) == KMS_BUTTON_NONE);
    CHECK(button_click(&b, 0x100u) == KMS_BUTTON_NONE);
    CHECK(button_click(&b, 0x200u) == KMS_BUTTON_RESTORE_TRIGGER);
    unsigned shown = KMS_COLOR_OFF;
    const uint32_t period = KMS_LED_NOTICE_ON_MS + KMS_LED_NOTICE_OFF_MS;
    for (unsigned n = 0; n < KMS_LED_RECOVERY_FLASHES; ++n) {
        CHECK(kms_notice_color(n * period, KMS_LED_RECOVERY_COLOR, KMS_LED_RECOVERY_FLASHES, &shown) && shown == KMS_COLOR_BLUE);
        CHECK(kms_notice_color(n * period + KMS_LED_NOTICE_ON_MS, KMS_LED_RECOVERY_COLOR, KMS_LED_RECOVERY_FLASHES, &shown) && shown == KMS_COLOR_OFF);
    }
    CHECK(!kms_notice_color(KMS_LED_RECOVERY_FLASHES * period, KMS_LED_RECOVERY_COLOR, KMS_LED_RECOVERY_FLASHES, &shown));
    return 0;
}
static int keyboard_led_tests(void)
{
    static kms_hid_parser_t p;
    static const uint8_t standard[] = {
        0x05,1,0x09,6,0xa1,1,0x05,7,0x09,4,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,
        0x75,7,0x95,1,0x81,1,0x05,8,0x19,1,0x29,5,0x75,1,0x95,5,0x91,2,
        0x75,3,0x95,1,0x91,1,0xc0};
    CHECK(kms_hid_parse(&p, standard, sizeof(standard)) && p.input_bits[0] == 8 && p.output_bits[0] == 8);
    uint8_t data[KMS_REPORT_BYTES + 1]; memset(data, 0xa5, sizeof(data));
    for (unsigned mask = 0; mask <= KMS_LOCK_MASK; ++mask) {
        CHECK(kms_hid_led_report(&p, 0, mask, data, 1) == 1 && data[0] == mask && data[1] == 0xa5);
    }
    CHECK(!kms_hid_led_report(&p, 1, 2, data, sizeof(data)));
    CHECK(!kms_hid_led_report(&p, 0, 2, data, 0));
    static const uint8_t numbered[] = {
        0x05,1,0x09,6,0xa1,1,0x85,7,0x05,7,0x09,4,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,
        0x75,7,0x95,1,0x91,1,0x05,8,0x09,2,0x75,2,0x95,1,0x91,2,
        0x85,8,0x09,3,0x75,1,0x95,1,0x91,2,0x75,7,0x91,1,0xc0};
    CHECK(kms_hid_parse(&p, numbered, sizeof(numbered)));
    CHECK(kms_hid_led_report(&p, 7, KMS_LOCK_CAPS, data, 3) == 3 && data[0] == 7 && data[1] == 0x80 && data[2] == 0);
    CHECK(!kms_hid_led_report(&p, 7, KMS_LOCK_CAPS, data, 2));
    CHECK(kms_hid_led_report(&p, 8, KMS_LOCK_CAPS | KMS_LOCK_SCROLL, data, 2) == 2 && data[0] == 8 && data[1] == 1);
    uint8_t mixed[sizeof(standard) + 11];
    memcpy(mixed, standard, sizeof(standard) - 1);
    const uint8_t vendor[] = {0x06,0,0xff,0x09,1,0x75,8,0x95,1,0x91,2};
    memcpy(mixed + sizeof(standard) - 1, vendor, sizeof(vendor)); mixed[sizeof(mixed) - 1] = 0xc0;
    CHECK(kms_hid_parse(&p, mixed, sizeof(mixed)) && p.count == 1);
    CHECK(!kms_hid_led_report_supported(&p, 0)); // A shared vendor field must never be zeroed.
    uint8_t array[sizeof(standard)]; memcpy(array, standard, sizeof(array)); array[37] = 0;
    CHECK(kms_hid_parse(&p, array, sizeof(array)) && p.count == 1 && !kms_hid_led_report_supported(&p, 0));
    CHECK(kms_hid_parse(&p, standard, sizeof(standard)));
    CHECK(!kms_hid_led_report(&p, 0, 2, NULL, sizeof(data)));
    CHECK(!kms_hid_led_report_supported(NULL, 0));

    kms_led_state_t state = {0}; kms_led_state_set(&state, true, KMS_LOCK_CAPS);
    CHECK(state.valid && state.leds == KMS_LOCK_CAPS && state.sequence == 1);
    kms_led_state_set(&state, true, KMS_LOCK_CAPS | 0xe0);
    CHECK(state.sequence == 1 && state.leds == KMS_LOCK_CAPS);
    kms_led_state_set(&state, false, KMS_LOCK_CAPS);
    CHECK(!state.valid && !state.leds && state.sequence == 2);
    kms_led_state_set(&state, false, 0); CHECK(state.sequence == 2);
    state.sequence = 0xffffffffu; kms_led_state_set(&state, true, 0);
    CHECK(state.valid && state.sequence == 0);
    kms_packet_t packet = {.type = KMS_STATUS, .slot = 2, .session = 22, .receiver = 11, .generation = 3,
        .flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE, .keyboard_leds = {.sequence = 5, .valid = true, .leds = KMS_LOCK_CAPS}};
    kms_led_feedback_t f = {0};
    CHECK(kms_led_feedback_accept(&f, &packet, 11, 22, 2, 3) && f.state.leds == KMS_LOCK_CAPS);
    CHECK(!kms_led_feedback_accept(&f, &packet, 11, 22, 2, 3));
    packet.keyboard_leds.sequence = 4; packet.keyboard_leds.leds = 0;
    CHECK(!kms_led_feedback_accept(&f, &packet, 11, 22, 2, 3) && f.state.leds == KMS_LOCK_CAPS);
    packet.keyboard_leds.sequence = 6;
    CHECK(!kms_led_feedback_accept(&f, &packet, 11, 22, 1, 3));
    CHECK(!kms_led_feedback_accept(&f, &packet, 12, 22, 2, 3));
    CHECK(!kms_led_feedback_accept(&f, &packet, 11, 23, 2, 3));
    CHECK(!kms_led_feedback_accept(&f, &packet, 11, 22, 2, 4));
    packet.flags = KMS_FLAG_USB_READY;
    CHECK(!kms_led_feedback_accept(&f, &packet, 11, 22, 2, 3));
    packet.flags |= KMS_FLAG_ACTIVE; packet.type = KMS_ACK; packet.session = 11; packet.receiver = 22;
    CHECK(kms_led_feedback_accept(&f, &packet, 11, 22, 2, 3) && !f.state.leds && f.state.valid);
    packet.keyboard_leds.sequence = 7; packet.keyboard_leds.valid = false;
    CHECK(kms_led_feedback_accept(&f, &packet, 11, 22, 2, 3) && !f.state.valid);
    packet.type = KMS_INPUT; ++packet.keyboard_leds.sequence;
    CHECK(!kms_led_feedback_accept(&f, &packet, 11, 22, 2, 3));
    f.state.sequence = 0xffffffffu; packet.type = KMS_ACK; packet.keyboard_leds.sequence = 0;
    CHECK(kms_led_feedback_accept(&f, &packet, 11, 22, 2, 3));
    uint8_t wire[KMS_WIRE_SIZE]; kms_packet_t decoded;
    packet.keyboard_leds = (kms_led_state_t){.sequence = 0x12345678, .valid = true, .leds = KMS_LOCK_MASK};
    CHECK(kms_wire_encode(&packet, wire, sizeof(wire)) == sizeof(wire));
    CHECK(kms_wire_decode(&decoded, wire, sizeof(wire)) && decoded.keyboard_leds.valid && decoded.keyboard_leds.leds == KMS_LOCK_MASK && decoded.keyboard_leds.sequence == 0x12345678);
    wire[KMS_WIRE_SIZE - 5] |= 0x20; CHECK(!kms_wire_decode(&decoded, wire, sizeof(wire)));
    wire[KMS_WIRE_SIZE - 5] &= (uint8_t)~0x20;
    wire[4] = 1; CHECK(!kms_wire_decode(&decoded, wire, sizeof(wire)));
    CHECK(!kms_wire_decode(&decoded, wire, sizeof(wire) - 5));
    return 0;
}

__declspec(dllexport) int kms_run_tests(void)
{
    checks = 0;
    int controls_result = controls_tests(); if (controls_result) return controls_result;
    int led_result = keyboard_led_tests(); if (led_result) return led_result;
    reset(); double_ctrl(1); CHECK(router.mode != 0);
    key(0, 0, 0x24, 200); CHECK(selected == 7); CHECK(!router.mode);
    CHECK(!kms_key(&emitted[count - 1], 0x24));
    key(0, 0, 0, 220); key(0, 0, 4, 230); CHECK(kms_key(&emitted[count - 1], 4));
    reset(); double_ctrl(0x10); key(0, 0, 0x59, 200); CHECK(selected == 1);
    reset(); double_ctrl(1); count = 0; kms_router_tick(&router, 3150);
    CHECK(count == 4); CHECK(emitted[0].modifiers == 1); CHECK(emitted[1].modifiers == 0);
    CHECK(emitted[2].modifiers == 1 && emitted[3].modifiers == 0);
    reset(); double_ctrl(1); count = 0; key(0, 0, 0x29, 200);
    CHECK(count == 1 && emitted[0].modifiers == 0 && !kms_key(&emitted[0], 0x29));
    reset(); double_ctrl(1); count = 0; key(0, 0, 4, 200);
    CHECK(count == 5 && kms_key(&emitted[4], 4));
    reset(); double_ctrl(1); key(0, 0, 0x27, 200); CHECK(selected == 0); CHECK(kms_key(&emitted[count - 1], 0x27));
    reset(); double_ctrl(1); key(0, 0, 0x13, 200); key(0, 0, 0, 210); key(0, 0, 0x20, 220); CHECK(paired == 3);
    reset(); double_ctrl(1); online = false; key(0, 0, 0x1e, 200);
    CHECK(selected == 1 && !kms_key(&emitted[count - 1], 0x1e));
    reset(); key(0, 1, 0, 1); key(0, 1, 6, 20); CHECK(emitted[count - 1].modifiers == 1); CHECK(kms_key(&emitted[count - 1], 6));
    key(0, 0, 0, 30); CHECK(!router.mode);
    reset(); key(0, 1, 0, 1); kms_router_tick(&router, 351); CHECK(emitted[count - 1].modifiers == 1);
    key(0, 0, 0, 400); CHECK(emitted[count - 1].modifiers == 0 && !router.mode);
    reset(); key(0, 1, 0, 1); key(0, 0, 0, 30); key(1, 1, 0, 60); key(1, 0, 0, 90); CHECK(!router.mode);
    reset(); key(0, 0, 4, 1); key(1, 0, 4, 2); key(0, 0, 0, 3); CHECK(kms_key(&emitted[count - 1], 4));
    kms_router_remove(&router, 1); CHECK(!kms_key(&emitted[count - 1], 4));
    reset(); key(0, 1, 0, 1); kms_sample_t mouse = {0}; mouse.state.buttons = 1;
    kms_router_input(&router, 1, &mouse, 10); CHECK(emitted[count - 1].modifiers == 1 && emitted[count - 1].buttons == 1);
    kms_router_block_held(&router); mouse.x = 1; kms_router_input(&router, 1, &mouse, 20);
    CHECK(!emitted[count - 1].modifiers && !emitted[count - 1].buttons);

    static kms_hid_parser_t parser;
    static const uint8_t keyboard[] = {
        0x05,1,0x09,6,0xa1,1,0x05,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
        0x75,8,0x95,1,0x81,1,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,6,0x81,0,0xc0};
    CHECK(kms_hid_parse(&parser, keyboard, sizeof(keyboard)));
    uint8_t report[] = {1,0,4,5,0,0,0,0}; kms_sample_t sample;
    CHECK(kms_hid_decode(&parser, report, sizeof(report), &sample));
    CHECK(sample.state.modifiers == 1 && kms_key(&sample.state, 4) && kms_key(&sample.state, 5));
    CHECK(!kms_hid_decode(&parser, report, 2, &sample));
    report[2] = 1; CHECK(!kms_hid_decode(&parser, report, sizeof(report), &sample));
    static const uint8_t consumer[] = {0x05,0x0c,0x09,1,0xa1,1,0x85,3,0x15,0,0x26,0xff,3,0x19,0,0x2a,0xff,3,0x75,16,0x95,1,0x81,0,0xc0};
    CHECK(kms_hid_parse(&parser, consumer, sizeof(consumer)));
    uint8_t media[] = {3,0xe9,0}; CHECK(kms_hid_decode(&parser, media, sizeof(media), &sample));
    CHECK(sample.state.consumer[0] == 0xe9); media[0] = 2; CHECK(!kms_hid_decode(&parser, media, sizeof(media), &sample));
    // A zero selector releases an array even when its usage/logical minimum is
    // nonzero. Dropping this valid null report leaves the media key held.
    static const uint8_t consumer_null[] = {
        0x05,0x0c,0x09,1,0xa1,1,0x85,3,0x15,1,0x26,0xff,3,
        0x19,1,0x2a,0xff,3,0x75,16,0x95,1,0x81,0,0xc0};
    CHECK(kms_hid_parse(&parser, consumer_null, sizeof(consumer_null)));
    media[0] = 3; media[1] = 0xe9;
    CHECK(kms_hid_decode(&parser, media, sizeof(media), &sample) && sample.state.consumer[0] == 0xe9);
    media[1] = 0;
    CHECK(kms_hid_decode(&parser, media, sizeof(media), &sample));
    CHECK(!sample.state.consumer[0]);
    media[1] = 0xff; media[2] = 0xff;
    CHECK(kms_hid_decode(&parser, media, sizeof(media), &sample) && !sample.state.consumer[0]);
    static const uint8_t button_array[] = {
        0x05,1,0x09,2,0xa1,1,0x05,9,0x15,1,0x25,3,0x19,1,0x29,3,
        0x75,8,0x95,1,0x81,0,0xc0};
    uint8_t selector = 2;
    CHECK(kms_hid_parse(&parser, button_array, sizeof(button_array)));
    CHECK(kms_hid_decode(&parser, &selector, 1, &sample) && sample.state.buttons == 2);
    selector = 0;
    CHECK(kms_hid_decode(&parser, &selector, 1, &sample) && !sample.state.buttons);
    static const uint8_t system_array[] = {
        0x05,1,0x09,0x80,0xa1,1,0x15,1,0x25,3,0x19,0x81,0x29,0x83,
        0x75,8,0x95,1,0x81,0,0xc0};
    selector = 1;
    CHECK(kms_hid_parse(&parser, system_array, sizeof(system_array)));
    CHECK(kms_hid_decode(&parser, &selector, 1, &sample) && sample.state.system == 1);
    selector = 0;
    CHECK(kms_hid_decode(&parser, &selector, 1, &sample) && !sample.state.system);
    static const uint8_t relative[] = {0x05,1,0x09,2,0xa1,1,0x85,1,0x09,0x30,0x09,0x31,0x16,0,0xf8,0x26,0xff,7,0x75,12,0x95,2,0x81,6,0xc0};
    CHECK(kms_hid_parse(&parser, relative, sizeof(relative)));
    uint8_t move[] = {1,0xff,0x2f,0}; CHECK(kms_hid_decode(&parser, move, sizeof(move), &sample));
    CHECK(sample.x == -1 && sample.y == 2);
    uint8_t gamepad[sizeof(relative)]; memcpy(gamepad, relative, sizeof(relative)); gamepad[3] = 5;
    CHECK(!kms_hid_parse(&parser, gamepad, sizeof(gamepad)));
    CHECK(!kms_hid_parse(&parser, relative, sizeof(relative) - 1));
    uint8_t invalid[] = {0x05}; CHECK(!kms_hid_parse(&parser, invalid, 1));
    uint8_t oversized[] = {0x05,7,0x19,0,0x29,0xff,0x75,32,0x96,0xff,0xff,0x81,2};
    CHECK(!kms_hid_parse(&parser, oversized, sizeof(oversized)));
    uint32_t rng = 42;
    uint8_t fuzz[96];
    for (unsigned n = 0; n < 10000; ++n) {
        for (unsigned j = 0; j < sizeof(fuzz); ++j) { rng = rng * 1664525u + 1013904223u; fuzz[j] = (uint8_t)(rng >> 24); }
        if (kms_hid_parse(&parser, fuzz, n % sizeof(fuzz) + 1)) {
            (void)kms_hid_decode(&parser, fuzz, sizeof(fuzz), &sample);
            uint8_t leds[KMS_REPORT_BYTES];
            for (unsigned id = 0; id < 256; ++id)
                (void)kms_hid_led_report(&parser, id, KMS_LOCK_MASK, leds, sizeof(leds));
        }
    }

    kms_packet_t packet = {.type=KMS_INPUT,.slot=7,.session=123,.receiver=456,.generation=9,.sequence=0xffffffffu};
    packet.state.modifiers = 0x11; packet.state.consumer[0] = 0xe2; kms_set_key(&packet.state, 127, true);
    packet.motion[0] = 0xfffffffeu; uint8_t wire[KMS_WIRE_SIZE]; kms_packet_t decoded;
    CHECK(kms_wire_encode(&packet, wire, sizeof(wire)) == KMS_WIRE_SIZE);
    CHECK(kms_wire_decode(&decoded, wire, sizeof(wire))); CHECK(decoded.receiver == 456 && decoded.slot == 7);
    CHECK(kms_state_equal(&packet.state, &decoded.state) && decoded.motion[0] == 0xfffffffeu);
    CHECK(!kms_wire_decode(&decoded, wire, sizeof(wire) - 1)); wire[6] = 8;
    CHECK(!kms_wire_decode(&decoded, wire, sizeof(wire))); CHECK(kms_newer(1, 0xffffffffu));

    kms_receiver_guard_t guard = {0}; bool deliver;
    packet.type = KMS_ACTIVATE; packet.sequence = 1; packet.motion[0] = 0;
    CHECK(kms_receiver_accept(&guard, &packet, 1, &sample, &deliver) && deliver && guard.active);
    CHECK(kms_receiver_accept(&guard, &packet, 2, &sample, &deliver) && !deliver);
    packet.type = KMS_INPUT; packet.sequence = 2; packet.motion[0] = 12;
    CHECK(kms_receiver_accept(&guard, &packet, 3, &sample, &deliver) && deliver && sample.x == 12);
    // The poll began before this packet was processed. Its older clock sample
    // must not expire the fresh lease through unsigned subtraction underflow.
    CHECK(!kms_receiver_expire(&guard, 2) && guard.active);
    CHECK(kms_receiver_accept(&guard, &packet, 4, &sample, &deliver) && !deliver);
    packet.sequence = 1; CHECK(!kms_receiver_accept(&guard, &packet, 5, &sample, &deliver));
    packet.sequence = 3; --packet.generation; CHECK(!kms_receiver_accept(&guard, &packet, 6, &sample, &deliver));
    ++packet.generation; CHECK(kms_receiver_expire(&guard, 1003) && !guard.active);
    CHECK(!kms_receiver_accept(&guard, &packet, 1004, &sample, &deliver));
    packet.type = KMS_ACTIVATE; ++packet.generation;
    CHECK(kms_receiver_accept(&guard, &packet, 1005, &sample, &deliver));
    packet.type = KMS_RELEASE; ++packet.sequence;
    CHECK(kms_receiver_accept(&guard, &packet, 1006, &sample, &deliver) && deliver && !guard.active);
    CHECK(!kms_key(&sample.state, 127));
    // An old generation cannot reactivate the previous computer after a switch.
    packet.type = KMS_ACTIVATE; --packet.generation; ++packet.sequence;
    CHECK(!kms_receiver_accept(&guard, &packet, 1007, &sample, &deliver));
    // A new master boot session is allowed once the old destination is inactive.
    ++packet.session; packet.generation = 1;
    CHECK(kms_receiver_accept(&guard, &packet, 1008, &sample, &deliver) && guard.active);
    ++packet.session;
    CHECK(!kms_receiver_accept(&guard, &packet, 1009, &sample, &deliver));
    guard.last_seen = 0xfffffff0u;
    CHECK(!kms_receiver_expire(&guard, guard.last_seen + KMS_LEASE_MS - 1) && guard.active);
    CHECK(kms_receiver_expire(&guard, guard.last_seen + KMS_LEASE_MS) && !guard.active);
    // Apply the same deadline rule to packets queued during the current poll.
    CHECK(!kms_time_elapsed(100, 101, KMS_RADIO_MAX_AGE_MS));
    CHECK(!kms_time_elapsed(100 + KMS_RADIO_MAX_AGE_MS - 1, 100, KMS_RADIO_MAX_AGE_MS));
    CHECK(kms_time_elapsed(100 + KMS_RADIO_MAX_AGE_MS, 100, KMS_RADIO_MAX_AGE_MS));
    return 0;
}
__declspec(dllexport) int kms_test_count(void) { return checks; }
