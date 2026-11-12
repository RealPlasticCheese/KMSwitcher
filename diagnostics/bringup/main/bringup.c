#include <inttypes.h>
#include <stdbool.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "kms_settings.h"

static const char *TAG = "kmswitcher";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "KMSwitcher %s: toolchain bring-up", KMS_ROLE);
    ESP_LOGI(TAG, "Target: %s; cores: %d; revision: %d; ESP-IDF: %s",
             CONFIG_IDF_TARGET, chip.cores, chip.revision, esp_get_idf_version());

    while (true) {
        ESP_LOGI(TAG, "Ready; free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(KMS_DIAGNOSTIC_LOG_MS));
    }
}
