#pragma once
#include "kms_core.h"
#include "kms_jiggler.h"
#include "kms_text.h"
#define KMS_NVS_NAMESPACE "kmswitcher"

// Bind once on the application task before starting callback workers. Wake may
// run on any task; wait is owned by the application and retains a 2 ms fallback.
void kms_app_init(void);
void kms_app_wake(void);
void kms_app_wait(void);

// All link APIs run on the application task. Wi-Fi callbacks only copy to a queue.
typedef bool (*kms_deliver_fn)(const kms_sample_t *samples, unsigned count);
void kms_link_init(bool master, kms_deliver_fn deliver, void (*clear)(void));
void kms_link_set_key_trace(kms_key_trace_fn);
// Set once at controller startup; 0 keeps automatic first-available selection.
void kms_link_set_boot_slot(unsigned slot);
void kms_link_set_jiggler_settings(uint16_t seconds, uint8_t distance);
bool kms_link_toggle_jiggler(void *ctx);
bool kms_link_cycle_jiggler_mode(void *ctx);
typedef struct {
    uint32_t boot, sequence;
    uint16_t seconds;
    uint8_t distance, mode;
    bool enabled, pending, modes_supported;
} kms_jiggler_status_t;
// NULL on success; otherwise a stable console/UI error code. No route change.
const char *kms_link_jiggler_get(unsigned slot, kms_jiggler_status_t *);
const char *kms_link_jiggler_set(unsigned slot, bool enabled);
const char *kms_link_jiggler_set_mode(unsigned slot, unsigned mode);
bool kms_link_jiggler_motion(kms_jiggler_motion_t *);
void kms_link_jiggler_moved(void);
void kms_link_jiggler_activity(void);
void kms_link_usb_fault(void);
void kms_link_input_overflow(void);
void kms_link_tick(bool usb_ready);
void kms_link_emit(void *, const kms_state_t *, int32_t, int32_t, int32_t, int32_t);
bool kms_link_select(void *, unsigned slot);
// Automatic handoffs require a reachable dongle with a mounted USB host.
// Mounted/suspended hosts remain eligible for USB wake; manual selection is broader.
bool kms_link_desktop_available(unsigned slot);
void kms_link_pair(void *, unsigned slot);
void kms_link_status(void);
size_t kms_link_radio_status(uint8_t *, size_t capacity);
bool kms_link_recovery_status(unsigned slot, uint8_t out[48]);
const char *kms_link_unpair(unsigned slot);
bool kms_link_has_pair(unsigned slot);
void kms_link_pause(void);
void kms_link_resume(void);
void kms_link_controller_recovery_tick(void);
bool kms_link_receiver_restart_in_progress(void);
void kms_link_set_usb_suspended(bool suspended);
bool kms_link_receiver_host_available(void);
// Drop unsent/stale input, release held output and reactivate the same route.
void kms_link_clear_input(void);
unsigned kms_link_selected(void);
bool kms_link_pairing(void);
bool kms_link_take_fault(void);
bool kms_link_error(void);
// Synthetic typing requires a healthy route and advances only after acknowledgement.
bool kms_link_typing_available(void);
bool kms_link_typing_ready(void);
void kms_link_text_begin(void);
bool kms_link_text_supported(void);
bool kms_link_text_send(const char *, size_t);
void kms_link_text_cancel(void);
// Receiver USB serializer: reports are acknowledged after final USB key-up.
bool kms_link_text_next(kms_sample_t *);
void kms_link_text_delivered(void);
bool kms_link_text_active(void);
// Receiver: snapshot copied on the application task; master: selected PC only.
void kms_link_set_keyboard_leds(const kms_led_state_t *);
bool kms_link_keyboard_leds(uint8_t *leds);
uint32_t kms_now(void);

void kms_board_init(void);
void kms_board_set_preferences(const kms_user_preferences_t *);
// A debounced long press is returned once, until the button is released.
enum kms_indicator { KMS_LED_NORMAL, KMS_LED_SELECT, KMS_LED_PAIR, KMS_LED_ERROR, KMS_LED_CONFIG, KMS_LED_JIGGLER };
bool kms_board_tick(enum kms_indicator, unsigned selected);
void kms_board_enable_controller_controls(void);
enum kms_button_event kms_board_take_event(void);
void kms_board_signal(enum kms_notice notice);
// Restart the failure blink phase at the same time as the link error window.
void kms_board_start_error(uint32_t now);
