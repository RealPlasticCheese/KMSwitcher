#include "kms_test_mode.h"
#include "kms_runtime.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

static bool controller, active, stored_known, history_written;
static uint32_t last_history;
static portMUX_TYPE mode_lock = portMUX_INITIALIZER_UNLOCKED;

void kms_test_mode_init(bool master)
{
    controller = master; active = history_written = false; stored_known = true; last_history = 0;
    if (!master) return;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(KMS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return;
    // Unreadable/corrupt state must not resume an interrupted test onto a PC.
    active = true; stored_known = false;
    if (result != ESP_OK) return;
    uint8_t value = 1; size_t bytes = sizeof(value);
    result = nvs_get_blob(handle, "test_mode", &value, &bytes);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) { active = false; stored_known = true; }
    else if (result == ESP_OK && bytes == sizeof(value) && value <= 1) {
        active = value != 0; stored_known = true;
    }
}

bool kms_test_mode_active(void)
{
    portENTER_CRITICAL(&mode_lock); bool value = active; portEXIT_CRITICAL(&mode_lock);
    return value;
}

bool kms_test_mode_set(bool value)
{
    if (value == kms_test_mode_active() && stored_known) return true;
    if (controller) {
        nvs_handle_t handle;
        esp_err_t result = nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (result != ESP_OK) return false;
        uint8_t stored = value ? 1 : 0;
        result = nvs_set_blob(handle, "test_mode", &stored, sizeof(stored));
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
        if (result != ESP_OK) return false;
    }
    portENTER_CRITICAL(&mode_lock);
    active = value; stored_known = true; history_written = false;
    portEXIT_CRITICAL(&mode_lock);
    return true;
}

bool kms_test_mode_history_allowed(uint32_t now)
{
    portENTER_CRITICAL(&mode_lock);
    bool allowed = !active || !history_written || now - last_history >= 60000u;
    if (active && allowed) { history_written = true; last_history = now; }
    portEXIT_CRITICAL(&mode_lock);
    return allowed;
}
