#include "preferences.h"
#include "kms_management.h"
#include "kms_remote.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "preferences";
static kms_user_preferences_t current;
static uint32_t revision;
void kms_preferences_init(uint32_t revision_seed)
{
    kms_user_preferences_defaults(&current); revision = revision_seed ? revision_seed : 1;
    nvs_handle_t h;
    esp_err_t result = nvs_open(KMS_NVS_NAMESPACE, NVS_READONLY, &h);
    if (result == ESP_ERR_NVS_NOT_FOUND) return;
    if (result != ESP_OK) { ESP_LOGW(TAG, "Cannot open preferences: %s", esp_err_to_name(result)); return; }
    uint8_t data[KMS_PREFERENCES_BYTES]; size_t bytes = sizeof(data);
    result = nvs_get_blob(h, "preferences", data, &bytes);
    if (result != ESP_OK || !kms_user_preferences_decode(&current, data, bytes)) {
        // Preserve the user's existing trigger when installing preferences-capable firmware.
        uint8_t legacy;
        if (nvs_get_u8(h, "trigger", &legacy) == ESP_OK && kms_trigger_valid(legacy)) current.trigger = legacy;
        if (result != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "Saved preferences unavailable/invalid; defaults and legacy trigger used");
    }
    nvs_close(h);
}
const kms_user_preferences_t *kms_preferences_get(void) { return &current; }
uint32_t kms_preferences_revision(void) { return revision; }
kms_trigger_t kms_preferences_trigger(void) { return (kms_trigger_t)current.trigger; }
bool kms_preferences_set(const kms_user_preferences_t *p)
{
    if (kms_remote_master_updating()) return false;
    uint8_t data[KMS_PREFERENCES_BYTES], old[KMS_PREFERENCES_BYTES];
    if (!kms_user_preferences_encode(p, data)) return false;
    if (kms_user_preferences_encode(&current, old) && !memcmp(data, old, sizeof(data))) return true;
    nvs_handle_t h;
    esp_err_t result = nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (result == ESP_OK) {
        result = nvs_set_blob(h, "preferences", data, sizeof(data));
        if (result == ESP_OK) result = nvs_commit(h);
        nvs_close(h);
    }
    if (result != ESP_OK) { ESP_LOGW(TAG, "Cannot save preferences: %s", esp_err_to_name(result)); return false; }
    current = *p; if (!++revision) ++revision; return true;
}
bool kms_preferences_save_trigger(void *ctx, kms_trigger_t trigger)
{
    (void)ctx;
    kms_user_preferences_t changed = current; changed.trigger = (uint8_t)trigger;
    return kms_preferences_set(&changed);
}
bool kms_preferences_save_boot_slot(void *ctx, unsigned slot)
{
    (void)ctx;
    if (slot > KMS_SLOTS) return false;
    kms_user_preferences_t changed = current; changed.boot_slot = (uint8_t)slot;
    return kms_preferences_set(&changed);
}
bool kms_preferences_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t result = nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (result == ESP_OK) {
        result = nvs_erase_all(h);
        if (result == ESP_OK) result = nvs_commit(h);
        nvs_close(h);
    }
    if (result != ESP_OK) ESP_LOGW(TAG, "Factory reset failed: %s", esp_err_to_name(result));
    return result == ESP_OK;
}
