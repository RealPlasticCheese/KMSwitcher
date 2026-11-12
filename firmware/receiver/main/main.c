#include "kms_runtime.h"
#include "kms_remote.h"
#include "usb_device.h"
#include "kms_test.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    kms_board_init(); kms_link_init(false, kms_usb_device_submit_many, kms_usb_device_clear);
    kms_test_init(esp_random(), false); kms_usb_device_init();
    while (true) {
        if (kms_remote_take_restart() || kms_test_take_restart()) {
            ESP_LOGI("receiver", "Restarting; releasing USB input before reset");
            kms_usb_device_clear();
            // Complete the USB feature transfer before detaching the device.
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        kms_usb_device_tick();
        kms_link_set_usb_suspended(kms_usb_device_suspended());
        kms_led_state_t leds; kms_usb_device_keyboard_leds(&leds); kms_link_set_keyboard_leds(&leds);
        kms_link_tick(kms_usb_device_ready() && kms_test_usb_available() && !kms_remote_restart_pending());
        if (kms_usb_device_take_fault()) kms_link_usb_fault();
        enum kms_indicator led = kms_link_error() ? KMS_LED_ERROR : kms_link_pairing() ? KMS_LED_PAIR : KMS_LED_NORMAL;
        if (kms_board_tick(led, kms_link_selected())) kms_link_pair(NULL, 0);
        kms_app_wait();
    }
}
