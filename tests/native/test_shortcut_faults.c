// Local command recognition and held-output safety across delivery discontinuities.
#include "kms_core.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
typedef struct { kms_trigger_t trigger; uint8_t modifier; unsigned key; } trigger_t;
static const trigger_t triggers[] = {
    {KMS_TRIGGER_CTRL, KMS_HID_CTRL_LEFT, 0}, {KMS_TRIGGER_CTRL, KMS_HID_CTRL_RIGHT, 0},
    {KMS_TRIGGER_ALT, KMS_HID_ALT_LEFT, 0}, {KMS_TRIGGER_ALT, KMS_HID_ALT_RIGHT, 0},
    {KMS_TRIGGER_SHIFT, KMS_HID_SHIFT_LEFT, 0}, {KMS_TRIGGER_SHIFT, KMS_HID_SHIFT_RIGHT, 0},
    {KMS_TRIGGER_SCROLL_LOCK, 0, KMS_HID_KEY_SCROLL_LOCK}, {KMS_TRIGGER_INSERT, 0, KMS_HID_KEY_INSERT},
    {KMS_TRIGGER_PAUSE, 0, KMS_HID_KEY_PAUSE}, {KMS_TRIGGER_NUM_LOCK, 0, KMS_HID_KEY_NUM_LOCK}
};
static kms_router_t router;
static kms_state_t observed, last;
static uint32_t now;
static unsigned checks, selections, selected, paired, saves;
static bool available;
static void emit(void *ctx, const kms_state_t *s, int32_t x, int32_t y, int32_t wheel, int32_t pan)
{ (void)ctx; (void)x; (void)y; (void)wheel; (void)pan; last = *s; kms_state_merge(&observed, s); }
static bool select_slot(void *ctx, unsigned slot)
{ (void)ctx; ++selections; if (available) selected = slot; return available; }
static void pair_slot(void *ctx, unsigned slot) { (void)ctx; paired = slot; }
static bool save_boot(void *ctx, unsigned slot) { (void)ctx; ++saves; selected = slot; return true; }
static bool configure(void *ctx, kms_trigger_t trigger)
{ (void)ctx; ++saves; selected = trigger; return true; }
static void setup(const trigger_t *trigger, uint32_t start)
{
    kms_router_init(&router, emit, select_slot, pair_slot, NULL);
    (void)kms_router_set_trigger(&router, trigger->trigger);
    router.configure = configure; router.save_boot_slot = save_boot;
    memset(&observed, 0, sizeof(observed)); memset(&last, 0, sizeof(last));
    now = start; selections = selected = paired = saves = 0; available = true;
}
static void input(unsigned owner, uint8_t modifier, unsigned key, unsigned second)
{
    kms_sample_t sample = {0}; sample.state.modifiers = modifier;
    if (key) kms_set_key(&sample.state, key, true);
    if (second) kms_set_key(&sample.state, second, true);
    kms_router_input(&router, owner, &sample, now); now += 10;
}
static void edge(const trigger_t *trigger, unsigned owner, unsigned stage)
{ input(owner, stage % 2 ? 0 : trigger->modifier, stage % 2 ? 0 : trigger->key, 0); }
static bool neutral(const kms_state_t *s)
{ kms_state_t zero = {0}; return kms_state_equal(s, &zero); }
static void prefix(const trigger_t *trigger, unsigned owner)
{ for (unsigned stage = 0; stage < 4; ++stage) edge(trigger, owner, stage); }
static int shortcut_matrix(void)
{
    const unsigned owners[] = {0, KMS_INPUTS - 1};
    for (unsigned t = 0; t < sizeof(triggers) / sizeof(triggers[0]); ++t)
        for (unsigned o = 0; o < 2; ++o)
            for (unsigned boundary = 0; boundary < 5; ++boundary)
                for (unsigned slot = 1; slot <= KMS_SLOTS; ++slot)
                    for (unsigned keypad = 0; keypad < 2; ++keypad)
                        for (unsigned wrap = 0; wrap < 2; ++wrap) {
                            const trigger_t *trigger = &triggers[t]; unsigned owner = owners[o];
                            setup(trigger, wrap ? 0xfffffff0u : 1000);
                            for (unsigned stage = 0; stage <= 4; ++stage) {
                                if (stage == boundary) {
                                    uint32_t deadline = router.mode ? router.deadline : router.inputs[owner].deadline;
                                    for (unsigned repeat = 0; repeat < 3; ++repeat) kms_router_block_output(&router);
                                    CHECK((router.mode ? router.deadline : router.inputs[owner].deadline) == deadline);
                                }
                                if (stage < 4) edge(trigger, owner, stage);
                            }
                            unsigned digit = (keypad ? KMS_HID_KEYPAD_1 : KMS_HID_KEY_1) + slot - 1;
                            input(owner, 0, digit, 0); input(owner, 0, 0, 0);
                            CHECK(selections == 1 && selected == slot && neutral(&observed));
                            input(owner, 0, 4, 0); CHECK(kms_key(&last, 4));
                        }
    return 0;
}
static int overlap_matrix(void)
{
    for (unsigned t = 0; t < sizeof(triggers) / sizeof(triggers[0]); ++t)
        for (unsigned boundary = 0; boundary < 4; ++boundary)
            for (unsigned overlap = 0; overlap < 2; ++overlap)
                for (unsigned release_trigger = 0; release_trigger < 2; ++release_trigger) {
                    const trigger_t *trigger = &triggers[t]; setup(trigger, 1000);
                    for (unsigned stage = 0; stage <= 3; ++stage) {
                        if (stage == boundary) kms_router_block_output(&router);
                        if (stage < 3) edge(trigger, 0, stage);
                    }
                    input(0, overlap ? trigger->modifier : 0,
                          overlap ? trigger->key : 0, KMS_HID_KEY_1 + 2);
                    CHECK(selected == 3 && selections == 1 && neutral(&observed));
                    if (overlap) input(0, release_trigger ? 0 : trigger->modifier,
                                       release_trigger ? KMS_HID_KEY_1 + 2 : trigger->key, 0);
                    else input(0, 0, KMS_HID_KEY_1 + 2, 0);
                    input(0, 0, 0, 0); CHECK(neutral(&observed));
                }
    return 0;
}
static int cancellation_and_safety(void)
{
    const trigger_t *trigger = &triggers[7];
    for (unsigned stage = 0; stage < 4; ++stage) {
        setup(trigger, 1000);
        for (unsigned n = 0; n <= stage; ++n) edge(trigger, 0, n);
        kms_router_block_output(&router);
        uint32_t deadline = router.mode ? router.deadline : router.inputs[0].deadline;
        kms_router_tick(&router, deadline);
        now = deadline + 1; input(0, 0, 0, 0);
        CHECK(!selections && !router.mode && neutral(&observed));
        // A fresh, incomplete tap after neutral still behaves as ordinary Insert.
        now = deadline + 20; edge(trigger, 0, 0); edge(trigger, 0, 1);
        kms_router_tick(&router, router.inputs[0].deadline);
        CHECK(kms_key(&observed, KMS_HID_KEY_INSERT));
    }
    for (unsigned reason = 0; reason < 3; ++reason) {
        setup(trigger, 1000); prefix(trigger, 0); kms_router_block_output(&router);
        if (reason == 0) input(0, 0, KMS_HID_KEY_ESCAPE, 0);
        else if (reason == 1) { available = false; input(0, 0, KMS_HID_KEY_1, 0); }
        else { now = router.deadline; kms_router_tick(&router, now); }
        CHECK(!selected && !router.mode && neutral(&observed));
    }
    // Unrelated typing cancels the retained command without replaying Insert.
    setup(trigger, 1000); prefix(trigger, 0); kms_router_block_output(&router);
    input(0, 0, 4, 0);
    CHECK(!selections && !router.mode && kms_key(&last, 4) && !kms_key(&observed, KMS_HID_KEY_INSERT));
    // Delivery loss cannot join taps from two different keyboards or modifiers.
    setup(trigger, 1000); edge(trigger, 0, 0); edge(trigger, 0, 1);
    kms_router_block_output(&router); edge(trigger, 1, 2); edge(trigger, 1, 3);
    input(1, 0, KMS_HID_KEY_1, 0); CHECK(!selections);
    setup(&triggers[0], 1000); edge(&triggers[0], 0, 0); edge(&triggers[0], 0, 1);
    kms_router_block_output(&router); edge(&triggers[1], 0, 2); edge(&triggers[1], 0, 3);
    input(0, 0, KMS_HID_KEY_1, 0); CHECK(!selections);
    // Output safety covers every held-state category on a different report source.
    setup(trigger, 1000);
    kms_sample_t held = {0}; held.state.modifiers = KMS_HID_SHIFT_LEFT;
    kms_set_key(&held.state, 4, true); held.state.buttons = 3;
    held.state.system = 1; held.state.consumer[0] = 0xe9;
    kms_router_input(&router, 1, &held, now);
    memset(&observed, 0, sizeof(observed)); kms_router_block_output(&router);
    held.x = 1; kms_router_input(&router, 1, &held, ++now);
    CHECK(neutral(&last));
    kms_router_input(&router, 1, &held, ++now); CHECK(neutral(&last));
    input(1, 0, 0, 0); prefix(trigger, 0);
    input(0, 0, KMS_HID_KEY_1 + 1, 0); CHECK(selected == 2 && neutral(&observed));
    input(0, 0, 0, 0); kms_router_input(&router, 1, &held, ++now);
    CHECK(kms_state_equal(&last, &held.state));
    // Physical discontinuities still cancel an incomplete local command.
    for (unsigned stage = 0; stage < 4; ++stage)
        for (unsigned remove = 0; remove < 2; ++remove) {
            setup(trigger, 1000);
            for (unsigned n = 0; n <= stage; ++n) edge(trigger, 0, n);
            if (remove) kms_router_remove(&router, 0); else kms_router_block_held(&router);
            input(0, 0, 0, 0); input(0, 0, KMS_HID_KEY_1, 0);
            CHECK(!selections && !router.mode);
        }
    // A reset cannot extend the double-tap or selection deadline.
    for (unsigned selection = 0; selection < 2; ++selection)
        for (unsigned offset = 0; offset < 3; ++offset) {
            setup(trigger, 0xffffff00u);
            if (selection) prefix(trigger, 0); else { edge(trigger, 0, 0); edge(trigger, 0, 1); }
            uint32_t deadline = selection ? router.deadline : router.inputs[0].deadline;
            kms_router_block_output(&router); now = deadline - 1 + offset;
            if (!selection) { edge(trigger, 0, 2); edge(trigger, 0, 3); }
            input(0, 0, KMS_HID_KEY_1, 0);
            CHECK((selections == 1) == (offset == 0));
        }
    return 0;
}
static int local_modes(void)
{
    const trigger_t *trigger = &triggers[7];
    setup(trigger, 1000); prefix(trigger, 0); input(0, 0, KMS_HID_KEY_P, 0);
    input(0, 0, 0, 0); kms_router_block_output(&router); input(0, 0, KMS_HID_KEY_1 + 2, 0);
    CHECK(paired == 3 && neutral(&observed));
    setup(trigger, 1000); prefix(trigger, 0); input(0, 0, KMS_HID_KEY_D, 0);
    input(0, 0, 0, 0); kms_router_block_output(&router); input(0, 0, KMS_HID_KEY_1 + 2, 0);
    CHECK(saves == 1 && selected == 3 && neutral(&observed));
    setup(trigger, 1000); prefix(trigger, 0); input(0, 0, KMS_HID_KEY_C, 0); input(0, 0, 0, 0);
    input(0, KMS_HID_CTRL_LEFT, 0, 0); kms_router_block_output(&router); input(0, 0, 0, 0);
    CHECK(saves == 1 && selected == KMS_TRIGGER_CTRL && neutral(&observed));
    return 0;
}
__declspec(dllexport) int kms_run_shortcut_fault_tests(void)
{
    checks = 0;
    int failure = shortcut_matrix(); if (failure) return failure;
    failure = overlap_matrix(); if (failure) return failure;
    failure = cancellation_and_safety(); if (failure) return failure;
    return local_modes();
}
__declspec(dllexport) int kms_shortcut_fault_test_count(void) { return checks; }
