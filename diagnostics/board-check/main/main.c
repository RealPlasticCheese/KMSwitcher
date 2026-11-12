#include "kms_runtime.h"
#include "board_profiles.h"
#include "pair_crypto.h"
#include <string.h>
#include <inttypes.h>
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

void app_main(void)
{
#if CONFIG_IDF_TARGET_ESP32S2
    void kms_diagnostic_console_init(void);
    kms_diagnostic_console_init();
#endif
    esp_chip_info_t chip; esp_chip_info(&chip);
    ESP_LOGI("diagnostic", "%s revision %u; IDF %s", CONFIG_IDF_TARGET, chip.revision, esp_get_idf_version());
#if CONFIG_IDF_TARGET_ESP32S2
    ESP_LOGI("diagnostic", "S2 blue LED GPIO: %d (-1 means disabled)", KMS_S2_LED_GPIO);
#endif
    // Verify the exact PSA APIs and curve used by enrollment on each chip.
    kms_exchange_t a = {0}, b = {0};
    uint8_t ka[KMS_LINK_KEY_BYTES], kb[KMS_LINK_KEY_BYTES], proof[KMS_NONCE_BYTES];
    const uint8_t nonce[KMS_NONCE_BYTES] = {1}, ma[6] = {2}, mb[6] = {4};
    ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
    bool crypto = kms_exchange_start(&a) && kms_exchange_start(&b) &&
        kms_exchange_key(&a, b.public_key, nonce, ma, mb, 1, true, ka) &&
        kms_exchange_key(&b, a.public_key, nonce, ma, mb, 1, false, kb) &&
        !memcmp(ka, kb, sizeof(ka)) && kms_pair_proof(ka, nonce, 1, false, proof);
    kms_exchange_end(&a); kms_exchange_end(&b);
    ESP_LOGI("diagnostic", "X25519 / SHA-256 / HMAC self-test: %s", crypto ? "PASS" : "FAIL");
    ESP_ERROR_CHECK(crypto ? ESP_OK : ESP_FAIL);
    kms_board_init(); uint32_t last = 0;
    while (true) {
        uint32_t now = kms_now();
        unsigned color = now / KMS_DIAGNOSTIC_STEP_MS % KMS_SLOTS + 1;
        if (kms_board_tick(KMS_LED_NORMAL, color)) ESP_LOGI("diagnostic", "Long press detected");
        if (now - last >= KMS_DIAGNOSTIC_LOG_MS) {
            last = now; ESP_LOGI("diagnostic", "Alive; heap=%" PRIu32 "; colour=%u", esp_get_free_heap_size(), color);
        }
        vTaskDelay(pdMS_TO_TICKS(KMS_MAIN_POLL_MS));
    }
}
