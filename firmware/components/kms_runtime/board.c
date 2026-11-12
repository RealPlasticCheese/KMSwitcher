#include "kms_runtime.h"
#include "kms_jiggler.h"
#include "board_profiles.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "led_strip.h"
static led_strip_handle_t rgb;
#define BUTTON_GPIO KMS_S3_BUTTON_GPIO
#define BUTTON_ACTIVE_LOW KMS_S3_BUTTON_ACTIVE_LOW
#else
#define BUTTON_GPIO KMS_S2_BUTTON_GPIO
#define BUTTON_ACTIVE_LOW KMS_S2_BUTTON_ACTIVE_LOW
#endif

static kms_button_t button;
static kms_user_preferences_t preferences;
static enum kms_button_event button_event;
static enum kms_notice signal;
static uint32_t signal_at;
static uint32_t error_at;
uint32_t kms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
void kms_board_init(void)
{
    kms_user_preferences_defaults(&preferences);
    kms_button_init(&button, false, kms_now());
    gpio_config_t c = {.pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
                       .pull_up_en = BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                       .pull_down_en = BUTTON_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&c));
#if CONFIG_IDF_TARGET_ESP32S3
    if (KMS_S3_RGB_GPIO >= 0) {
        led_strip_config_t strip = {.strip_gpio_num = KMS_S3_RGB_GPIO, .max_leds = 1,
                                    .led_model = KMS_S3_RGB_MODEL, .color_component_format = KMS_S3_RGB_FORMAT};
        led_strip_rmt_config_t rmt = {.resolution_hz = KMS_RGB_RESOLUTION_HZ};
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &rgb));
        ESP_ERROR_CHECK(led_strip_clear(rgb));
    }
#else
    if (KMS_S2_LED_GPIO >= 0) {
        gpio_reset_pin(KMS_S2_LED_GPIO);
        gpio_set_direction(KMS_S2_LED_GPIO, GPIO_MODE_OUTPUT);
    }
#endif
}
void kms_board_set_preferences(const kms_user_preferences_t *p)
{ if (kms_user_preferences_valid(p)) preferences = *p; }
void kms_board_enable_controller_controls(void) { kms_button_init(&button, true, kms_now()); }
enum kms_button_event kms_board_take_event(void)
{ enum kms_button_event e = button_event; button_event = KMS_BUTTON_NONE; return e; }
void kms_board_signal(enum kms_notice notice) { signal = notice; signal_at = kms_now(); }
void kms_board_start_error(uint32_t now) { error_at = now; }
bool kms_board_tick(enum kms_indicator mode, unsigned selected)
{
    uint32_t now = kms_now();
    bool was_down = button.stable, was_ready = button.factory_ready;
    enum kms_button_event event = kms_button_update(&button, gpio_get_level(BUTTON_GPIO) != BUTTON_ACTIVE_LOW, now);
    if (event != KMS_BUTTON_NONE) button_event = event;
    if (was_down != button.stable)
        ESP_LOGI("board", "Button %s (GPIO%d)", button.stable ? "pressed" : "released", BUTTON_GPIO);
    if (!was_ready && button.factory_ready) ESP_LOGW("board", "Factory reset armed; release BOOT to confirm");
    unsigned interval = mode == KMS_LED_PAIR ? KMS_LED_PAIRING_BLINK_MS :
                        mode == KMS_LED_CONFIG ? KMS_LED_CONFIG_BLINK_MS :
                        mode == KMS_LED_ERROR ? KMS_LED_FAILURE_BLINK_MS : KMS_LED_SELECTION_BLINK_MS;
    uint32_t elapsed = mode == KMS_LED_ERROR ? now - error_at : now;
    bool normal = mode == KMS_LED_NORMAL || mode == KMS_LED_JIGGLER;
    bool light = normal || (elapsed / interval) % 2 == 0;
#if CONFIG_IDF_TARGET_ESP32S3
    const uint8_t *slots = preferences.slot_colors;
    unsigned color = mode == KMS_LED_PAIR ? KMS_LED_PAIRING_COLOR : mode == KMS_LED_SELECT ? KMS_LED_SELECTION_COLOR :
                     mode == KMS_LED_CONFIG ? KMS_LED_CONFIG_COLOR : mode == KMS_LED_ERROR ? KMS_LED_FAILURE_COLOR :
                     selected && selected <= KMS_SLOTS ? slots[selected - 1] : KMS_LED_IDLE_COLOR;
    unsigned shown = light ? color : KMS_COLOR_OFF;
    // Diagnostics deliberately cycle slot colours without enabling controller controls.
    if (button.controller && normal && !preferences.normal_led) shown = KMS_COLOR_OFF;
    if (button.controller && mode == KMS_LED_JIGGLER) shown = kms_jiggler_indicator(now, shown);
    if (signal != KMS_NOTICE_NONE) {
        color = signal == KMS_NOTICE_RESTORED ? KMS_LED_RECOVERY_COLOR : signal == KMS_NOTICE_SAVED ? KMS_LED_CONFIG_SAVED_COLOR :
                signal == KMS_NOTICE_CANCELLED ? KMS_LED_CONFIG_CANCEL_COLOR : KMS_LED_FAILURE_COLOR;
        unsigned flashes = signal == KMS_NOTICE_RESTORED ? KMS_LED_RECOVERY_FLASHES : signal == KMS_NOTICE_SAVED ? KMS_LED_CONFIG_SAVED_FLASHES :
                           signal == KMS_NOTICE_CANCELLED ? KMS_LED_CONFIG_CANCEL_FLASHES : KMS_LED_SAVE_ERROR_FLASHES;
        if (!kms_notice_color(now - signal_at, color, flashes, &shown)) signal = KMS_NOTICE_NONE;
    }
    if (button.controller && !kms_time_elapsed(now, button.started, KMS_LED_STARTUP_MS)) shown = KMS_LED_STARTUP_COLOR;
    if (button.factory_armed) shown = button.factory_ready ? KMS_LED_FACTORY_READY_COLOR :
        (now / KMS_LED_FACTORY_BLINK_MS) % 2 == 0 ? KMS_LED_FACTORY_HOLD_COLOR : KMS_COLOR_OFF;
    static unsigned previous = ~0u, previous_brightness;
    unsigned b = preferences.brightness;
    if (rgb && (shown != previous || b != previous_brightness)) {
        led_strip_set_pixel(rgb, 0, shown & KMS_COLOR_RED ? b : 0,
                            shown & KMS_COLOR_GREEN ? b : 0, shown & KMS_COLOR_BLUE ? b : 0);
        led_strip_refresh(rgb); previous = shown; previous_brightness = b;
    }
#else
    if (KMS_S2_LED_GPIO >= 0) gpio_set_level(KMS_S2_LED_GPIO, light ^ KMS_S2_LED_ACTIVE_LOW);
#endif
    return event == KMS_BUTTON_LONG;
}
