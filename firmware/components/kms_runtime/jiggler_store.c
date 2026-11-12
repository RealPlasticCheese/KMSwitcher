#include "kms_jiggler_store.h"
#include "kms_management.h"
#include "kms_runtime.h"
#include "sdkconfig.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#if !CONFIG_IDF_TARGET_ESP32S3
static const char *TAG = "jiggler-store";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t desired[16];
static uint32_t revision, attempted_at;
static bool dirty, attempted, warned;
static void encode(const kms_jiggler_t *j, uint8_t data[16])
{
    memset(data, 0, 16); memcpy(data, "KJG1", 4);
    data[4] = j->enabled; data[5] = j->mode;
    data[6] = (uint8_t)j->interval_seconds; data[7] = (uint8_t)(j->interval_seconds >> 8); data[8] = j->distance;
    kms_put_u32(data + 12, kms_crc32(data, 12));
}
void kms_jiggler_store_load(kms_jiggler_t *j, uint32_t now)
{
    *j = (kms_jiggler_t){.interval_seconds = KMS_JIGGLER_DEFAULT_SECONDS, .distance = 1, .changed = now};
    nvs_handle_t h; uint8_t data[16]; size_t size = sizeof(data);
    esp_err_t error = nvs_open(KMS_NVS_NAMESPACE, NVS_READONLY, &h);
    if (error == ESP_OK) { error = nvs_get_blob(h, "jiggler", data, &size); nvs_close(h); }
    if (error == ESP_OK && size == 16 && !memcmp(data, "KJG1", 4) && data[4] <= 1 &&
        data[5] < KMS_JIGGLER_MODE_COUNT && (data[6] || data[7]) && data[8] && data[8] <= KMS_JIGGLER_MAX_DISTANCE &&
        !data[9] && !data[10] && !data[11] && kms_get_u32(data + 12) == kms_crc32(data, 12)) {
        j->enabled = data[4]; j->mode = data[5]; j->interval_seconds = (uint16_t)(data[6] | data[7] << 8); j->distance = data[8];
        j->random = 0x6d2b79f5u ^ now;
        ESP_LOGI(TAG, "KMSJIGGLER restored enabled=%u seconds=%u distance=%u mode=%u", j->enabled, j->interval_seconds, j->distance, j->mode);
    } else if (error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "KMSJIGGLER saved configuration unavailable/invalid; using disabled defaults");
    }
    encode(j, desired); dirty = attempted = warned = false; revision = 0;
}
void kms_jiggler_store_update(const kms_jiggler_t *j)
{
    uint8_t data[16]; encode(j, data);
    portENTER_CRITICAL(&lock);
    if (memcmp(data, desired, sizeof(data))) { memcpy(desired, data, sizeof(data)); dirty = true; ++revision; }
    portEXIT_CRITICAL(&lock);
}
bool kms_jiggler_store_flush(void)
{
    uint8_t data[16];
    portENTER_CRITICAL(&lock);
    bool save = dirty; uint32_t version = revision; memcpy(data, desired, sizeof(data));
    portEXIT_CRITICAL(&lock);
    if (!save) return true;
    attempted = true; attempted_at = kms_now();
    nvs_handle_t h; esp_err_t error = nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (error == ESP_OK) {
        error = nvs_set_blob(h, "jiggler", data, sizeof(data));
        if (error == ESP_OK) error = nvs_commit(h);
        nvs_close(h);
    }
    portENTER_CRITICAL(&lock);
    if (error == ESP_OK && version == revision) dirty = false;
    portEXIT_CRITICAL(&lock);
    if (error == ESP_OK) {
        warned = false;
        ESP_LOGI(TAG, "KMSJIGGLER configuration saved enabled=%u mode=%u", data[4], data[5]);
    } else if (!warned) {
        warned = true; ESP_LOGW(TAG, "KMSJIGGLER save failed; current setting remains in RAM; retrying");
    }
    return error == ESP_OK;
}
void kms_jiggler_store_maintain(void)
{
    if (!attempted || kms_time_elapsed(kms_now(), attempted_at, 1000u)) (void)kms_jiggler_store_flush();
}
#else
void kms_jiggler_store_load(kms_jiggler_t *j, uint32_t now) { (void)j; (void)now; }
void kms_jiggler_store_update(const kms_jiggler_t *j) { (void)j; }
void kms_jiggler_store_maintain(void) { }
bool kms_jiggler_store_flush(void) { return true; }
#endif
