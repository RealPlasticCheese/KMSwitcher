#include "preferences.h"
#include "kms_management.h"
#include "nvs.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, writes, erases;
static bool exists, staged, legacy_present, fail_open, fail_write, fail_commit, updating;
static uint8_t stored[32], pending[32], legacy;
const char *esp_err_to_name(esp_err_t error) { (void)error; return "fake NVS error"; }
bool kms_remote_master_updating(void) { return updating; }
esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *h)
{ (void)space; (void)mode; *h = 1; return fail_open ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; staged = false; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{
    (void)h; (void)key;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < sizeof(stored)) return ESP_ERR_NVS_INVALID_LENGTH;
    *size = sizeof(stored); memcpy(data, stored, *size); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    (void)h;
    if (fail_write || size != sizeof(pending) || memcmp(key, "preferences", 12)) return ESP_FAIL;
    memcpy(pending, data, size); staged = true; ++writes; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{ (void)h; (void)key; if (!legacy_present) return ESP_ERR_NVS_NOT_FOUND; *out = legacy; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (fail_commit) return ESP_FAIL;
    if (staged) { memcpy(stored, pending, sizeof(stored)); exists = true; }
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; ++erases; exists = legacy_present = false; return ESP_OK; }
static void input(kms_router_t *r, uint8_t mods, uint32_t now)
{ kms_sample_t sample = {0}; sample.state.modifiers = mods; kms_router_input(r, 0, &sample, now); }
__declspec(dllexport) unsigned kms_preferences_test_echo(const uint8_t *data, unsigned size, uint8_t *out)
{
    kms_user_preferences_t p;
    return kms_user_preferences_decode(&p, data, size) && kms_user_preferences_encode(&p, out) ? KMS_PREFERENCES_BYTES : 0;
}
__declspec(dllexport) int kms_run_preferences_tests(void)
{
    kms_user_preferences_t defaults, changed, decoded;
    uint8_t wire[32], original[32];
    kms_user_preferences_defaults(&defaults); CHECK(kms_user_preferences_valid(&defaults));
    CHECK(defaults.trigger == KMS_TRIGGER_DEFAULT && defaults.double_ms == KMS_DOUBLE_MS && defaults.select_ms == KMS_SELECT_MS);
    CHECK(kms_user_preferences_encode(&defaults, wire)); memcpy(original, wire, sizeof(wire));
    CHECK(kms_user_preferences_decode(&decoded, wire, sizeof(wire)));
    CHECK(decoded.brightness == defaults.brightness && !memcmp(decoded.slot_colors, defaults.slot_colors, KMS_SLOTS));
    for (unsigned i = 0; i < sizeof(wire); ++i) {
        wire[i] ^= 1; CHECK(!kms_user_preferences_decode(&decoded, wire, sizeof(wire))); wire[i] ^= 1;
    }
    CHECK(!kms_user_preferences_decode(&decoded, wire, 31));
    changed = defaults; changed.double_ms = 99; CHECK(!kms_user_preferences_valid(&changed));
    changed.double_ms = 1001; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.select_ms = 499; CHECK(!kms_user_preferences_valid(&changed));
    changed.select_ms = 15001; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.brightness = 0; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.slot_colors[6] = 8; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.trigger = 0; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.normal_led = 2; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.boot_slot = KMS_SLOTS + 1; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.keyboard_leds = 2; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.jiggler_seconds = 0; CHECK(!kms_user_preferences_valid(&changed));
    changed = defaults; changed.jiggler_distance = 0; CHECK(!kms_user_preferences_valid(&changed));
    changed.jiggler_distance = 128; CHECK(!kms_user_preferences_valid(&changed));
    // Version 3 retains its interval and adopts the one-count distance.
    wire[4] = 3; wire[27] = 0; wire[10] = 7; wire[11] = 0; kms_put_u32(wire + 28, kms_crc32(wire, 28));
    CHECK(kms_user_preferences_decode(&decoded, wire, sizeof(wire)) && decoded.jiggler_seconds == 7 && decoded.jiggler_distance == 1);
    // Version 2 keeps its boot slot and adopts the new interval default.
    wire[4] = 2; wire[9] = 7; wire[10] = wire[11] = 0; kms_put_u32(wire + 28, kms_crc32(wire, 28));
    CHECK(kms_user_preferences_decode(&decoded, wire, sizeof(wire)) && decoded.boot_slot == 7 && decoded.jiggler_seconds == 30);
    wire[9] = 0;
    // Version 1 migrates without losing existing preferences.
    wire[4] = 1; wire[10] = wire[11] = 0; kms_put_u32(wire + 28, kms_crc32(wire, 28));
    CHECK(kms_user_preferences_decode(&decoded, wire, sizeof(wire)) && decoded.boot_slot == 0);
    CHECK(kms_user_preferences_encode(&decoded, wire) && wire[4] == 4);
    // First use imports the old trigger without changing any other preference or pairing key.
    legacy_present = true; legacy = KMS_TRIGGER_INSERT; kms_preferences_init(77);
    CHECK(kms_preferences_trigger() == KMS_TRIGGER_INSERT && kms_preferences_revision() == 77);
    CHECK(kms_preferences_get()->double_ms == KMS_DOUBLE_MS);
    changed = *kms_preferences_get(); changed.double_ms = 800; changed.select_ms = 9000;
    changed.jiggler_seconds = 65535; changed.jiggler_distance = 127; changed.boot_slot = 2; changed.brightness = 30; changed.keyboard_leds = 0; changed.slot_colors[0] = KMS_COLOR_CYAN;
    CHECK(kms_preferences_set(&changed)); CHECK(writes == 1 && !erases && kms_preferences_revision() == 78);
    CHECK(kms_preferences_set(&changed)); CHECK(writes == 1 && kms_preferences_revision() == 78);
    kms_preferences_init(100); CHECK(kms_preferences_get()->double_ms == 800 && kms_preferences_get()->brightness == 30);
    CHECK(kms_preferences_trigger() == KMS_TRIGGER_INSERT && kms_preferences_revision() == 100);
    CHECK(kms_preferences_get()->boot_slot == 2 && kms_preferences_get()->jiggler_seconds == 65535 && kms_preferences_get()->jiggler_distance == 127);
    // A physical trigger change preserves all other preferences across restart.
    CHECK(kms_preferences_save_trigger(NULL, KMS_TRIGGER_ALT)); kms_preferences_init(200);
    CHECK(kms_preferences_trigger() == KMS_TRIGGER_ALT && kms_preferences_get()->double_ms == 800);
    changed = *kms_preferences_get(); changed.brightness = 50;
    fail_open = true; CHECK(!kms_preferences_set(&changed)); fail_open = false;
    fail_write = true; CHECK(!kms_preferences_set(&changed)); fail_write = false;
    fail_commit = true; CHECK(!kms_preferences_set(&changed)); fail_commit = false;
    CHECK(kms_preferences_get()->brightness == 30 && kms_preferences_revision() == 200);
    updating = true; CHECK(!kms_preferences_set(&changed)); updating = false;
    // Keyboard/console setter persists only the boot slot, and failed writes retain it.
    CHECK(!kms_preferences_save_boot_slot(NULL, KMS_SLOTS + 1));
    CHECK(kms_preferences_save_boot_slot(NULL, 7)); kms_preferences_init(210);
    CHECK(kms_preferences_get()->boot_slot == 7 && kms_preferences_get()->double_ms == 800);
    fail_commit = true; CHECK(!kms_preferences_save_boot_slot(NULL, 1)); fail_commit = false;
    CHECK(kms_preferences_get()->boot_slot == 7 && kms_preferences_revision() == 210);
    CHECK(kms_preferences_save_boot_slot(NULL, 0)); kms_preferences_init(220);
    CHECK(kms_preferences_get()->boot_slot == 0);
    // Restoring preferences is a single preference-blob write; it never erases pairing/history.
    CHECK(kms_preferences_set(&defaults)); CHECK(!erases);
    kms_preferences_init(300); CHECK(kms_preferences_trigger() == KMS_TRIGGER_DEFAULT && kms_preferences_get()->brightness == defaults.brightness);
    stored[7] ^= 1; kms_preferences_init(301);
    CHECK(kms_preferences_trigger() == KMS_TRIGGER_INSERT && kms_preferences_get()->brightness == defaults.brightness);
    // Router timing changes apply without retaining a held key or stale shortcut mode.
    static kms_router_t router;
    kms_router_init(&router, NULL, NULL, NULL, NULL);
    kms_sample_t held = {0}; held.state.modifiers = KMS_HID_CTRL_LEFT;
    kms_router_input(&router, 0, &held, 1);
    changed = defaults; changed.double_ms = 1000; changed.select_ms = 15000;
    CHECK(kms_router_set_preferences(&router, &changed));
    CHECK(router.inputs[0].blocked.modifiers == KMS_HID_CTRL_LEFT && router.inputs[0].visible.modifiers == 0);
    input(&router, 0, 20); input(&router, KMS_HID_CTRL_LEFT, 100); input(&router, 0, 150);
    input(&router, KMS_HID_CTRL_LEFT, 700); input(&router, 0, 750);
    CHECK(router.mode == KMS_MODE_SELECT && router.deadline == 15750);
    kms_router_tick(&router, 15749); CHECK(router.mode == KMS_MODE_SELECT);
    kms_router_tick(&router, 15750); CHECK(router.mode == KMS_MODE_NORMAL);
    changed.double_ms = 0; CHECK(!kms_router_set_preferences(&router, &changed)); CHECK(router.double_ms == 1000);
    return 0;
}
__declspec(dllexport) int kms_preferences_test_count(void) { return checks; }
