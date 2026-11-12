#include "kms_core.h"
#include "kms_management.h"
#include "kms_jiggler.h"
#include <string.h>

_Static_assert(KMS_DOUBLE_MS >= 100 && KMS_DOUBLE_MS <= 1000, "Default double-tap window outside preference limits");
_Static_assert(KMS_SELECT_MS >= 500 && KMS_SELECT_MS <= 15000, "Default selection timeout outside preference limits");
_Static_assert(KMS_STATUS_LED_BRIGHTNESS >= 1 && KMS_STATUS_LED_BRIGHTNESS <= 255, "Default brightness outside preference limits");

void kms_user_preferences_defaults(kms_user_preferences_t *p)
{
    static const uint8_t colors[] = KMS_LED_SLOT_COLORS;
    _Static_assert(sizeof(colors) >= KMS_SLOTS, "Supply a colour for every slot");
    memset(p, 0, sizeof(*p));
    p->trigger = KMS_TRIGGER_DEFAULT; p->double_ms = KMS_DOUBLE_MS; p->select_ms = KMS_SELECT_MS;
    p->normal_led = KMS_LED_NORMAL_ENABLED; p->brightness = KMS_STATUS_LED_BRIGHTNESS;
    p->keyboard_leds = KMS_KEYBOARD_LEDS_ENABLED;
    p->jiggler_seconds = KMS_JIGGLER_DEFAULT_SECONDS;
    p->jiggler_distance = 1;
    memcpy(p->slot_colors, colors, KMS_SLOTS);
}
bool kms_user_preferences_valid(const kms_user_preferences_t *p)
{
    if (!p || !kms_trigger_valid(p->trigger) || p->double_ms < 100 || p->double_ms > 1000 ||
        p->select_ms < 500 || p->select_ms > 15000 || p->normal_led > 1 ||
        !p->brightness || p->keyboard_leds > 1 || p->boot_slot > KMS_SLOTS || !p->jiggler_seconds ||
        !p->jiggler_distance || p->jiggler_distance > KMS_JIGGLER_MAX_DISTANCE) return false;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) if (p->slot_colors[i] > KMS_COLOR_WHITE) return false;
    return true;
}
bool kms_user_preferences_encode(const kms_user_preferences_t *p, uint8_t *out)
{
    if (!out || !kms_user_preferences_valid(p)) return false;
    memset(out, 0, KMS_PREFERENCES_BYTES);
    memcpy(out, "KMP1", 4); out[4] = 4; out[5] = p->trigger; out[6] = p->normal_led;
    out[7] = p->brightness; out[8] = p->keyboard_leds; out[9] = p->boot_slot;
    out[10] = (uint8_t)p->jiggler_seconds; out[11] = (uint8_t)(p->jiggler_seconds >> 8);
    kms_put_u32(out + 12, p->double_ms); kms_put_u32(out + 16, p->select_ms);
    memcpy(out + 20, p->slot_colors, KMS_SLOTS);
    out[27] = p->jiggler_distance;
    kms_put_u32(out + 28, kms_crc32(out, 28)); return true;
}
bool kms_user_preferences_decode(kms_user_preferences_t *p, const uint8_t *data, size_t bytes)
{
    if (!p || !data || bytes != KMS_PREFERENCES_BYTES || memcmp(data, "KMP1", 4) || data[4] < 1 || data[4] > 4 ||
        (data[4] == 1 && data[9]) || (data[4] < 3 && (data[10] || data[11])) ||
        (data[4] < 4 && data[27]) || kms_crc32(data, 28) != kms_get_u32(data + 28)) return false;
    kms_user_preferences_t decoded = {0};
    decoded.trigger = data[5]; decoded.normal_led = data[6]; decoded.brightness = data[7];
    decoded.boot_slot = data[4] >= 2 ? data[9] : 0;
    decoded.jiggler_seconds = data[4] >= 3 ? (uint16_t)(data[10] | (uint16_t)data[11] << 8) : KMS_JIGGLER_DEFAULT_SECONDS;
    decoded.jiggler_distance = data[4] >= 4 ? data[27] : 1;
    decoded.keyboard_leds = data[8]; decoded.double_ms = kms_get_u32(data + 12);
    decoded.select_ms = kms_get_u32(data + 16); memcpy(decoded.slot_colors, data + 20, KMS_SLOTS);
    if (!kms_user_preferences_valid(&decoded)) return false;
    *p = decoded; return true;
}
