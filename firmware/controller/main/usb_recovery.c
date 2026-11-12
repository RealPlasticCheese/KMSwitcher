#include "usb_recovery.h"
#include "usb_input.h"
#include "usb_health.h"
#include "usb_devices.h"
#include "usb_leds.h"
#include "usb_fault.h"
#include "kms_runtime.h"
#include "kms_diagnostic_store.h"
#include "kms_test_mode.h"
#include <inttypes.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

enum recovery_state { RUNNING, DRAINING, ENUMERATING, RESTARTING, BLOCKED };
enum call_stage { POLLING = 1, QUIESCING, DISCONNECTING, CLEANUP, RECONNECTING, DISCOVERING };
typedef struct {
    uint32_t magic, check, restarts, slot, pending;
    kms_diag_record_t fault;
} restart_guard_t;
static RTC_NOINIT_ATTR restart_guard_t guard;
static portMUX_TYPE guard_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t call_timer;
static bool call_armed;
static unsigned call_stage;
static kms_diag_record_t call_context;
static enum recovery_state state;
static uint32_t phase_since, episode_since, window_since, stable_since;
static unsigned attempts, expected, recoveries;
static bool permitted, manual, boot_restore;
#ifdef CONFIG_KMS_TEST_SUPPORT
static bool test_block;
#endif
static const char *TAG = "usb-recovery";

static uint32_t guard_checksum(void)
{
    const kms_diag_record_t *f = &guard.fault;
    return ~(guard.magic ^ guard.restarts ^ guard.slot ^ guard.pending ^ f->version ^ f->sequence ^
             f->boot ^ f->uptime_ms ^ f->idle_ms ^ f->detail ^ f->repeats ^ (uint32_t)f->event << 16 ^ f->slot);
}
static void seal_guard(void)
{
    guard.magic = 0x4b555353u;
    guard.check = guard_checksum();
}
static void call_watchdog(void *arg)
{
    (void)arg;
    uint32_t now = kms_now(); bool restart = false;
    portENTER_CRITICAL(&guard_lock);
    if (call_armed && (!guard.restarts || guard.pending == 1) &&
        kms_time_elapsed(now, call_context.uptime_ms, KMS_USB_CALL_TIMEOUT_MS)) {
        guard.restarts = 1; guard.slot = call_context.slot; guard.pending = 2;
        guard.fault = call_context;
        guard.fault.uptime_ms = now;
        if (guard.fault.idle_ms) guard.fault.idle_ms += now - call_context.uptime_ms;
        guard.fault.detail |= call_stage << 8;
        seal_guard(); call_armed = false; restart = true;
    }
    portEXIT_CRITICAL(&guard_lock);
    // The input task may own USB/UART locks. Do not log or touch its router or
    // history model here. RTC carries the fault to the next boot for persistence.
    if (restart) esp_restart();
}
void kms_usb_recovery_watch_begin(bool allowed)
{
    kms_diag_record_t context;
    kms_usb_input_capture_event(&context, KMS_EVENT_INPUT_ERROR, kms_usb_input_selected(),
        KMS_USB_HOST_EVENT_TAG | KMS_USB_HOST_CALL_TIMEOUT << 16);
    portENTER_CRITICAL(&guard_lock);
    call_context = context;
    call_stage = state == DRAINING ? CLEANUP : state == ENUMERATING ? DISCOVERING : POLLING;
    call_armed = allowed && (!guard.restarts || guard.pending == 1);
    portEXIT_CRITICAL(&guard_lock);
}
void kms_usb_recovery_watch_end(void)
{
    portENTER_CRITICAL(&guard_lock); call_armed = false; portEXIT_CRITICAL(&guard_lock);
}
static void stage(unsigned value)
{
    portENTER_CRITICAL(&guard_lock); call_stage = value; portEXIT_CRITICAL(&guard_lock);
}
static void record(unsigned event, unsigned address, unsigned number)
{
    // Keep the existing persisted event/counter layout. 0x55 identifies USB-host
    // recovery; the remaining bytes are phase, USB address and interface.
    kms_usb_input_event(KMS_EVENT_INPUT_ERROR, kms_usb_input_selected(),
                          KMS_USB_HOST_EVENT_TAG | event << 16 | address << 8 | number);
}
void kms_usb_recovery_init(void)
{
    state = RUNNING; attempts = recoveries = 0; permitted = manual = false;
    phase_since = episode_since = window_since = stable_since = kms_now();
    call_armed = false;
    if (esp_reset_reason() != ESP_RST_SW || guard.magic != 0x4b555353u || guard.restarts > 1 ||
        guard.slot > KMS_SLOTS || guard.pending > 2 ||
        guard.check != guard_checksum()) {
        memset(&guard, 0, sizeof(guard)); seal_guard();
    }
    boot_restore = guard.pending != 0;
    if (guard.pending == 2) {
        kms_diagnostics_recover_event(&guard.fault);
        ESP_LOGE(TAG, "USB call blocked in stage=%" PRIu32 " at=%" PRIu32 "ms on boot=%08" PRIx32
                 "; guarded restart, destination=%" PRIu32, (guard.fault.detail >> 8) & 255u,
                 guard.fault.uptime_ms, guard.fault.boot, guard.slot);
    }
    guard.pending = 0; seal_guard();
    if (!call_timer) {
        esp_timer_create_args_t timer = {.callback = call_watchdog, .name = "usb-call-guard"};
        ESP_ERROR_CHECK(esp_timer_create(&timer, &call_timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(call_timer, KMS_USB_CALL_WATCH_MS * 1000u));
    }
}
unsigned kms_usb_recovery_boot_slot(unsigned configured_slot)
{
    return boot_restore ? guard.slot : configured_slot;
}
bool kms_usb_recovery_boot_paused(void) { return boot_restore && !guard.slot; }
bool kms_usb_recovery_block_initial(void) { return boot_restore || kms_usb_recovery_active(); }
bool kms_usb_recovery_active(void) { return state == DRAINING || state == ENUMERATING || state == RESTARTING; }
bool kms_usb_recovery_request(void)
{
    if (!permitted || kms_usb_recovery_active()) return false;
    manual = true; return true;
}
#ifdef CONFIG_KMS_TEST_SUPPORT
bool kms_usb_recovery_test_block(void)
{
    if (!kms_test_mode_active() || guard.restarts || !kms_usb_recovery_request()) return false;
    test_block = true; return true;
}
#endif
static void failed(void)
{
    record(KMS_USB_HOST_FAILED, 0, 0); state = BLOCKED;
    ESP_LOGE(TAG, "Automatic recovery stopped; check hub power/cable, then usb-recover or restart the S3");
}
static void restart_or_stop(uint32_t now)
{
    unsigned slot = kms_usb_input_selected(); bool used;
    portENTER_CRITICAL(&guard_lock);
    used = guard.restarts != 0;
    if (!used) { guard.restarts = 1; guard.slot = slot; guard.pending = 1; seal_guard(); }
    portEXIT_CRITICAL(&guard_lock);
    if (used) { failed(); return; }
    kms_usb_input_block(false);
    record(KMS_USB_HOST_RESTART, 0, 0); kms_usb_input_save();
    state = RESTARTING; phase_since = now;
    ESP_LOGE(TAG, "USB cleanup failed; one guarded S3 restart scheduled, destination=%" PRIu32, guard.slot);
}
static void start(uint32_t now, const kms_usb_overdue_t *overdue)
{
    if (manual || kms_time_elapsed(now, window_since, KMS_USB_RECOVERY_WINDOW_MS)) {
        attempts = 0; window_since = now;
    }
    manual = false;
    if (attempts >= KMS_USB_RECOVERY_ATTEMPTS) { failed(); return; }
    bool new_episode = !kms_usb_recovery_active();
    if (new_episode) {
        expected = kms_usb_devices_active(); episode_since = now;
        if (!expected) expected = 1;
    }
    ++attempts; state = DRAINING; phase_since = stable_since = now;
    if (new_episode) kms_usb_fault_capture(overdue);
    if (overdue) {
        record(KMS_USB_HOST_OVERDUE, overdue->address, overdue->interface_number);
        ESP_LOGW(TAG, "Unhealthy control operation=%u on %u/%u, age=%" PRIu32 "ms; pending=%u",
                 overdue->operation, overdue->address, overdue->interface_number,
                 now - overdue->started, kms_usb_health_pending());
    }
    // Publish state/history before entering any cleanup/SDK operation. The
    // independent call guard is already armed by the USB input tick.
    record(KMS_USB_HOST_RESET, 0, 0);
    stage(QUIESCING);
#ifdef CONFIG_KMS_TEST_SUPPORT
    // Explicit test sessions only: prove the independent timer/reset/RTC
    // history path while the real input task cannot make another tick.
    if (test_block) {
        test_block = false;
        if (kms_test_mode_active()) vTaskSuspend(NULL);
    }
#endif
    kms_usb_input_block(true);
    kms_usb_leds_quiesce(true); kms_usb_devices_quiesce(true);
    stage(DISCONNECTING);
    ESP_LOGW(TAG, "Disconnecting USB root port; attempt %u/%u", attempts, KMS_USB_RECOVERY_ATTEMPTS);
    if (usb_host_lib_set_root_port_power(false) != ESP_OK) restart_or_stop(now);
}
void kms_usb_recovery_tick(bool allowed)
{
    uint32_t now = kms_now(); permitted = allowed;
    if (!allowed) { phase_since = stable_since = now; return; }
    if (state == RESTARTING) {
        // Give the background history writer a bounded chance; never wait on NVS.
        if (kms_time_elapsed(now, phase_since, KMS_USB_RESTART_LOG_MS)) esp_restart();
        return;
    }
    if (state == DRAINING) {
        usb_host_lib_info_t host;
        if (kms_usb_devices_drained() && kms_usb_leds_drained() && !kms_usb_health_pending() &&
            usb_host_lib_info(&host) == ESP_OK && !host.num_devices &&
            kms_time_elapsed(now, phase_since, KMS_USB_RECOVERY_OFF_MS)) {
            kms_usb_devices_quiesce(false); kms_usb_leds_quiesce(false);
            kms_usb_health_clear_fault();
            stage(RECONNECTING);
            if (usb_host_lib_set_root_port_power(true) != ESP_OK) { restart_or_stop(now); return; }
            state = ENUMERATING; phase_since = now;
            ESP_LOGI(TAG, "USB references drained; root port enabled, rediscovering inputs");
        } else if (kms_time_elapsed(now, phase_since, KMS_USB_RECOVERY_DRAIN_MS)) restart_or_stop(now);
        return;
    }
    if (state == ENUMERATING) {
        if (kms_usb_devices_healthy() >= expected) {
            state = RUNNING; stable_since = now; ++recoveries; record(KMS_USB_HOST_RESTORED, 0, 0);
            ESP_LOGI(TAG, "USB recovered in %" PRIu32 "ms; destination=%u", now - episode_since, kms_usb_input_selected());
        } else if (kms_time_elapsed(now, phase_since, KMS_USB_RECOVERY_ENUM_MS)) {
            usb_host_lib_info_t host;
            if (!kms_usb_health_pending() && usb_host_lib_info(&host) == ESP_OK && !host.num_devices) {
                state = RUNNING; record(KMS_USB_HOST_ABSENT, 0, 0);
                ESP_LOGW(TAG, "No USB device present; waiting for hub power/cable instead of restarting");
            } else start(now, NULL);
        }
        return;
    }
    if (state == BLOCKED && !manual) return;
    kms_usb_overdue_t overdue;
    if (manual) { start(now, NULL); return; }
    if (kms_usb_health_overdue(now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue)) { start(now, &overdue); return; }
    if (!kms_usb_devices_healthy()) stable_since = now;
    else if (kms_time_elapsed(now, stable_since, KMS_USB_RECOVERY_WINDOW_MS)) {
        attempts = 0; window_since = now;
        portENTER_CRITICAL(&guard_lock); guard.restarts = 0; seal_guard(); portEXIT_CRITICAL(&guard_lock);
    }
}
void kms_usb_recovery_status(void)
{
    kms_usb_recovery_status_t snapshot;
    kms_usb_recovery_status_snapshot(&snapshot); kms_usb_recovery_print_status(&snapshot);
}
void kms_usb_recovery_status_snapshot(kms_usb_recovery_status_t *out)
{
    *out = (kms_usb_recovery_status_t){state, attempts, recoveries, 0};
    portENTER_CRITICAL(&guard_lock); out->restarts = guard.restarts; portEXIT_CRITICAL(&guard_lock);
}
void kms_usb_recovery_print_status(const kms_usb_recovery_status_t *snapshot)
{
    static const char *const names[] = {"running", "draining", "enumerating", "restarting", "blocked"};
    ESP_LOGI(TAG, "State=%s attempts=%u recovered=%u restart-budget-used=%" PRIu32 " control-timeout=%ums call-timeout=%ums",
             names[snapshot->state], snapshot->attempts, snapshot->recoveries, snapshot->restarts,
             KMS_USB_CONTROL_TIMEOUT_MS, KMS_USB_CALL_TIMEOUT_MS);
}
