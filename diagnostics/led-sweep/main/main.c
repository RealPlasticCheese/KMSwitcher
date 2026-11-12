#include <stdbool.h>
#include <stddef.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_sweep.h"
#include "sdkconfig.h"
#include "tusb.h"

#if !CONFIG_IDF_TARGET_ESP32S2 || !CONFIG_ESP_CONSOLE_NONE
#error LED sweep requires ESP32-S2 and the UART console disabled
#endif

static const char *TAG = "led-sweep";
static const gpio_num_t pins[] = KMS_LED_SWEEP_GPIOS;

_Static_assert(sizeof(pins) > 0, "Supply at least one candidate GPIO");
_Static_assert(KMS_LED_SWEEP_DWELL_MS > 0 && KMS_LED_SWEEP_REPEATS > 0 &&
               KMS_LED_SWEEP_GAP_MS > 0 && KMS_LED_SWEEP_START_MS > 0,
               "Sweep timings and repeat count must be positive");

void kms_diagnostic_console_init(void);

static void release_pin(gpio_num_t pin)
{
    // gpio_reset_pin alone enables a pull-up; explicitly float the input.
    ESP_ERROR_CHECK(gpio_reset_pin(pin));
    ESP_ERROR_CHECK(gpio_set_pull_mode(pin, GPIO_FLOATING));
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_INPUT));
}

static bool wait_connected(unsigned ms)
{
    TickType_t start = xTaskGetTickCount();
    while (tud_cdc_connected()) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(ms)) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static bool test_pin(gpio_num_t pin)
{
    ESP_LOGI(TAG, "Testing GPIO%d; %u LOW/HIGH cycles", pin, KMS_LED_SWEEP_REPEATS);
    // Minimum drive strength is not a current limiter: use only an unattached
    // dongle, and inspect unknown board connections before expanding the list.
    ESP_ERROR_CHECK(gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_0));
    bool connected = true;
    for (unsigned cycle = 1; cycle <= KMS_LED_SWEEP_REPEATS && connected; ++cycle) {
        for (unsigned level = 0; level <= 1; ++level) {
            if (!tud_cdc_connected()) { connected = false; break; }
            ESP_ERROR_CHECK(gpio_set_level(pin, level));
            ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_OUTPUT));
            ESP_LOGI(TAG, "GPIO%d %s (%u/%u); hold %u ms", pin,
                     level ? "HIGH" : "LOW", cycle, KMS_LED_SWEEP_REPEATS, KMS_LED_SWEEP_DWELL_MS);
            if (!wait_connected(KMS_LED_SWEEP_DWELL_MS)) { connected = false; break; }
        }
    }
    release_pin(pin);
    if (!connected) return false;
    ESP_LOGI(TAG, "GPIO%d released: input, no pulls", pin);
    return wait_connected(KMS_LED_SWEEP_GAP_MS);
}

void app_main(void)
{
    kms_diagnostic_console_init();
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_num_t pin = pins[i];
        // Keep exclusions enforced even if the configured candidate list changes.
        bool candidate = (pin >= 1 && pin <= 18) || pin == 21 || (pin >= 33 && pin <= 44);
        ESP_ERROR_CHECK(candidate && GPIO_IS_VALID_OUTPUT_GPIO(pin) ? ESP_OK : ESP_ERR_INVALID_ARG);
    }
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) release_pin(pins[i]);

    while (true) {
        // DTR must be asserted: serial-capture.py needs --dtr on this console.
        while (!tud_cdc_connected()) vTaskDelay(pdMS_TO_TICKS(20));
        ESP_LOGI(TAG, "ESP32-S2 LED sweep: GPIO1-18, 21, 33-44 (configured candidates only)");
        ESP_LOGI(TAG, "Skipped: BOOT/strapping 0/45/46; USB 19/20; flash 26-32; absent 22-25");
        ESP_LOGI(TAG, "LOW/HIGH are electrical levels, not assumed LED on/off states");
        ESP_LOGI(TAG, "Starting in %u ms; close console to release the current pin", KMS_LED_SWEEP_START_MS);
        if (!wait_connected(KMS_LED_SWEEP_START_MS)) continue;

        unsigned pass = 0;
        while (tud_cdc_connected()) {
            ESP_LOGI(TAG, "Pass %u starting", ++pass);
            bool complete = true;
            for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
                if (!test_pin(pins[i])) { complete = false; break; }
            }
            if (!complete) break;
            ESP_LOGI(TAG, "Pass %u complete. No LED change does not prove a power-only LED.", pass);
            if (!wait_connected(KMS_LED_SWEEP_START_MS)) break;
        }
    }
}
