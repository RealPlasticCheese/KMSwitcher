#include "usb_preferences.h"
#include "kms_runtime.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static QueueHandle_t saves;
static void save_worker(void *arg)
{
    (void)arg;
    kms_usb_policy_t policy;
    while (true) {
        if (xQueueReceive(saves, &policy, portMAX_DELAY) != pdTRUE) continue;
        nvs_handle_t handle;
        esp_err_t result = nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (result == ESP_OK) {
            result = nvs_set_blob(handle, "usb_policy", &policy, sizeof(policy));
            if (result == ESP_OK) result = nvs_commit(handle);
            nvs_close(handle);
        }
        if (result == ESP_OK) printf("KMSUSB SAVED\n");
        else ESP_LOGW("usb-input", "USB choices apply now but were not saved: %s", esp_err_to_name(result));
    }
}
void kms_usb_preferences_init(kms_usb_policy_t *policy)
{
    kms_usb_policy_reset(policy, true);
    nvs_handle_t handle;
    if (nvs_open(KMS_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        kms_usb_policy_t saved;
        size_t bytes = sizeof(saved);
        esp_err_t result = nvs_get_blob(handle, "usb_policy", &saved, &bytes);
        nvs_close(handle);
        if (result == ESP_OK && bytes == sizeof(saved) && kms_usb_policy_valid(&saved)) *policy = saved;
        else if (result != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW("usb-input", "USB choice storage unavailable or incompatible; using automatic input");
    }
    saves = xQueueCreate(1, sizeof(*policy));
    if (!saves || xTaskCreate(save_worker, "usb-prefs", 3072, NULL, 1, NULL) != pdPASS) {
        if (saves) vQueueDelete(saves);
        saves = NULL;
        ESP_LOGW("usb-input", "USB choice saving unavailable; runtime selection remains available");
    }
}
bool kms_usb_preferences_save(const kms_usb_policy_t *policy)
{
    return saves && xQueueOverwrite(saves, policy) == pdTRUE;
}
