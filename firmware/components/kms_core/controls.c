#include "kms_core.h"
#include <string.h>

void kms_button_init(kms_button_t *b, bool controller, uint32_t now)
{
    memset(b, 0, sizeof(*b)); b->controller = controller; b->started = now;
}
enum kms_button_event kms_button_update(kms_button_t *b, bool down, uint32_t now)
{
    if (b->clicks && kms_time_elapsed(now, b->click_started, KMS_BUTTON_MULTI_PRESS_MS)) b->clicks = 0;
    if (down != b->raw) { b->raw = down; b->changed = now; }
    if (b->raw != b->stable && kms_time_elapsed(now, b->changed, KMS_BUTTON_DEBOUNCE_MS)) {
        b->stable = b->raw;
        if (b->stable) {
            b->pressed = now; b->long_fired = false;
            b->factory_armed = b->controller && !kms_time_elapsed(now, b->started, KMS_FACTORY_RESET_WINDOW_MS);
            b->factory_ready = false;
        } else {
            bool reset = b->factory_ready;
            b->factory_armed = b->factory_ready = false;
            if (reset) { b->clicks = 0; return KMS_BUTTON_FACTORY_RESET; }
            if (b->controller && !kms_time_elapsed(now, b->pressed, KMS_BUTTON_SHORT_PRESS_MS)) {
                if (!b->clicks) b->click_started = now;
                if (++b->clicks == KMS_BUTTON_RECOVERY_PRESSES) {
                    b->clicks = 0; return KMS_BUTTON_RESTORE_TRIGGER;
                }
            } else b->clicks = 0;
        }
    }
    // Require a debounced continuous hold, with no pending release/bounce.
    if (b->stable && b->raw) {
        if (b->factory_armed) {
            if (kms_time_elapsed(now, b->pressed, KMS_FACTORY_RESET_HOLD_MS)) b->factory_ready = true;
        } else if (!b->long_fired && kms_time_elapsed(now, b->pressed, KMS_BUTTON_HOLD_MS)) {
            b->long_fired = true; return KMS_BUTTON_LONG;
        }
    }
    return KMS_BUTTON_NONE;
}
bool kms_notice_color(uint32_t elapsed, unsigned color, unsigned flashes, unsigned *shown)
{
    const uint32_t period = KMS_LED_NOTICE_ON_MS + KMS_LED_NOTICE_OFF_MS;
    if (elapsed / period >= flashes) return false;
    *shown = elapsed % period < KMS_LED_NOTICE_ON_MS ? color : KMS_COLOR_OFF;
    return true;
}
