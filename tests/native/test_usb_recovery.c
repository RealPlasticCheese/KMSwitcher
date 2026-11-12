// Production supervisor with real timing watches and deterministic host/reset I/O.
#define CONFIG_KMS_TEST_SUPPORT 1
#define kms_usb_input_event kms_diagnostics_event
#define kms_usb_input_selected kms_link_selected
#define kms_usb_input_capture_event kms_diagnostics_capture_event
#define kms_usb_input_save kms_diagnostics_save
#include "usb_recovery.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static uint32_t now;
static unsigned checks, off_calls, on_calls, restarts, neutral, blocks, events;
static unsigned active_inputs, healthy_inputs, host_devices, selected;
static bool input_drained, leds_drained, input_quiet, leds_quiet, power_error;
static int reset_reason;
static kms_router_t fake_router;
static esp_timer_create_args_t timer_config;
static bool block_quiesce, block_power, restarted_inside_call;
static bool test_mode;
static kms_diag_record_t recovered_fault;
static unsigned recovered_faults, last_event_phase;
static unsigned fault_captures;

esp_err_t esp_timer_create(const esp_timer_create_args_t *config, esp_timer_handle_t *out)
{ timer_config = *config; *out = &timer_config; return ESP_OK; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t interval)
{ (void)timer; (void)interval; return ESP_OK; }
void kms_diagnostics_capture_event(kms_diag_record_t *out, enum kms_event event, unsigned slot, uint32_t detail)
{ *out = (kms_diag_record_t){.version = 1, .boot = 1234, .uptime_ms = now, .idle_ms = 5000,
    .event = (uint16_t)event, .slot = (uint16_t)slot, .detail = detail, .repeats = 1}; }
void kms_diagnostics_recover_event(const kms_diag_record_t *record)
{ recovered_fault = *record; ++recovered_faults; }
static void blocked_call(void)
{
    // Dispatch the independent timer while the USB call is still on the stack;
    // no input/recovery tick or callback completion can make progress here.
    unsigned before = restarts; now += KMS_USB_CALL_TIMEOUT_MS;
    timer_config.callback(timer_config.arg);
    restarted_inside_call = restarts == before + 1;
}
bool kms_test_mode_active(void) { return test_mode; }
void vTaskSuspend(void *task) { (void)task; blocked_call(); }

uint32_t kms_now(void) { return now; }
bool kms_time_elapsed(uint32_t n, uint32_t since, uint32_t duration) { return n - since >= duration; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void kms_fake_check(int result) { (void)result; }
void kms_diagnostics_event(enum kms_event e, unsigned slot, uint32_t detail)
{ (void)e; (void)slot; last_event_phase = (detail >> 16) & 255u; ++events; }
void kms_diagnostics_save(void) { }
void kms_usb_fault_capture(const kms_usb_overdue_t *reason) { (void)reason; ++fault_captures; }
unsigned kms_link_selected(void) { return selected; }
void kms_link_clear_input(void) { ++neutral; }
void kms_router_block_held(kms_router_t *r) { (void)r; ++blocks; }
void kms_usb_input_block(bool clear) { kms_router_block_held(&fake_router); if (clear) kms_link_clear_input(); }
void kms_usb_devices_quiesce(bool value) { input_quiet = value; if (value && block_quiesce) blocked_call(); }
void kms_usb_leds_quiesce(bool value) { leds_quiet = value; }
bool kms_usb_devices_drained(void) { return input_drained; }
bool kms_usb_leds_drained(void) { return leds_drained; }
unsigned kms_usb_devices_active(void) { return active_inputs; }
unsigned kms_usb_devices_healthy(void) { return healthy_inputs; }
esp_err_t usb_host_lib_info(usb_host_lib_info_t *out)
{ memset(out, 0, sizeof(*out)); out->num_devices = host_devices; return ESP_OK; }
esp_err_t usb_host_lib_set_root_port_power(bool enabled)
{ if (enabled) ++on_calls; else ++off_calls; if (block_power) blocked_call(); return power_error ? ESP_FAIL : ESP_OK; }
int esp_reset_reason(void) { return reset_reason; }
void esp_restart(void) { ++restarts; }
static void reset(void)
{
    now = off_calls = on_calls = restarts = neutral = blocks = events = 0;
    active_inputs = 4; healthy_inputs = 4; host_devices = 3; selected = 3;
    input_drained = leds_drained = input_quiet = leds_quiet = power_error = false;
    block_quiesce = block_power = restarted_inside_call = false; recovered_faults = last_event_phase = 0;
    test_block = test_mode = false;
    fault_captures = 0;
    reset_reason = ESP_RST_POWERON; memset(&guard, 0, sizeof(guard));
    kms_usb_health_init(); kms_usb_recovery_init(); kms_usb_recovery_tick(true);
}
static void drain(void)
{
    input_drained = leds_drained = true; host_devices = 0;
    now += KMS_USB_RECOVERY_OFF_MS; kms_usb_recovery_tick(true);
}
static void reconnect(void)
{
    host_devices = 3; healthy_inputs = 4;
    now += KMS_USB_PROBE_MS; kms_usb_recovery_tick(true);
}
__declspec(dllexport) unsigned kms_usb_recovery_test_count(void) { return checks; }
__declspec(dllexport) int kms_run_usb_recovery_tests(void)
{
    static int owner;
    kms_usb_overdue_t overdue;
    // Recreate two silent receiver control pipes, varying operation, completion
    // ordering and clock wrap. No interrupt/input progress is assumed here.
    static int second_owner;
    checks = 0;
    for (unsigned operation = 0; operation < KMS_USB_OPERATIONS; ++operation)
        for (unsigned completion = 0; completion < 3; ++completion)
            for (unsigned wrap = 0; wrap < 2; ++wrap) {
                reset(); now = wrap ? 0xfffffe00u : 1000; kms_usb_recovery_init();
                kms_usb_recovery_tick(true);
                kms_usb_health_begin(&owner, operation, 2, 0);
                kms_usb_health_begin(&second_owner, KMS_USB_PROBE, 3, 0);
                now += KMS_USB_CONTROL_TIMEOUT_MS - 1; kms_usb_recovery_tick(true);
                CHECK(!off_calls && !fault_captures && !restarts);
                ++now;
                if (completion) kms_usb_health_complete(&owner, USB_TRANSFER_STATUS_COMPLETED, 10, 10);
                if (completion == 2) kms_usb_health_complete(&second_owner, USB_TRANSFER_STATUS_COMPLETED, 10, 10);
                kms_usb_recovery_tick(true);
                if (completion == 2) CHECK(!off_calls && !fault_captures && !kms_usb_recovery_active());
                else {
                    CHECK(off_calls == 1 && fault_captures == 1 && !restarts && selected == 3);
                    CHECK(kms_usb_recovery_active() && input_quiet && leds_quiet && neutral == 1);
                    kms_usb_health_complete(&owner, USB_TRANSFER_STATUS_CANCELED, 0, 10);
                    kms_usb_health_complete(&second_owner, USB_TRANSFER_STATUS_CANCELED, 0, 10);
                    drain(); reconnect();
                    CHECK(!kms_usb_recovery_active() && !restarts && selected == 3);
                }
            }
    reset(); now = 1800000; healthy_inputs = 0; kms_usb_recovery_tick(true);
    CHECK(off_calls == 0 && restarts == 0); // Long idle / absent devices are healthy silence.
    kms_usb_health_begin(&owner, KMS_USB_LED, 2, 0);
    now += KMS_USB_CONTROL_TIMEOUT_MS - 1; kms_usb_recovery_tick(true);
    CHECK(off_calls == 0);
    ++now; kms_usb_recovery_tick(true);
    CHECK(off_calls == 1 && neutral == 1 && blocks == 1 && input_quiet && leds_quiet);
    CHECK(kms_usb_recovery_active() && !kms_usb_recovery_request());
    now += KMS_USB_RECOVERY_OFF_MS; kms_usb_recovery_tick(true);
    CHECK(on_calls == 0); // Deadline alone cannot release SDK-owned storage.
    kms_usb_health_complete(&owner, 1, 0, 1); input_drained = true; host_devices = 0;
    kms_usb_recovery_tick(true); CHECK(on_calls == 0); // LED references must also drain.
    leds_drained = true; kms_usb_recovery_tick(true);
    CHECK(on_calls == 1 && !input_quiet && !leds_quiet && kms_usb_recovery_active());
    healthy_inputs = 2; host_devices = 3; kms_usb_recovery_tick(true);
    CHECK(kms_usb_recovery_active()); // Wait for both original input bundles.
    reconnect(); CHECK(!kms_usb_recovery_active() && recoveries == 1 && restarts == 0 && selected == 3);

    reset(); now = 0xfffffff0u; kms_usb_health_begin(&owner, KMS_USB_DESCRIPTOR, 7, 1);
    now += KMS_USB_CONTROL_TIMEOUT_MS - 1;
    CHECK(!kms_usb_health_overdue(now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue));
    ++now; kms_usb_recovery_tick(false); CHECK(off_calls == 0); // OTA/pairing/boot confirmation guard.
    kms_usb_recovery_tick(true); CHECK(off_calls == 1);
    CHECK(kms_usb_health_overdue(now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue) && overdue.address == 7);
    now += KMS_USB_RECOVERY_DRAIN_MS; kms_usb_recovery_tick(true);
    CHECK(state == RESTARTING && restarts == 0 && guard.restarts == 1);
    now += KMS_USB_RESTART_LOG_MS; kms_usb_recovery_tick(false); CHECK(restarts == 0);
    kms_usb_recovery_tick(true); CHECK(restarts == 0);
    now += KMS_USB_RESTART_LOG_MS; kms_usb_recovery_tick(true); CHECK(restarts == 1);
    reset_reason = ESP_RST_SW; kms_usb_health_init(); kms_usb_recovery_init();
    CHECK(kms_usb_recovery_boot_slot(2) == 3 && !kms_usb_recovery_boot_paused() && kms_usb_recovery_block_initial());
    kms_usb_recovery_tick(true); kms_usb_health_begin(&owner, KMS_USB_LED, 2, 0);
    now += KMS_USB_CONTROL_TIMEOUT_MS; kms_usb_recovery_tick(true);
    now += KMS_USB_RECOVERY_DRAIN_MS; kms_usb_recovery_tick(true);
    CHECK(state == BLOCKED && restarts == 1); // No warm-boot restart loop.
    CHECK(guard.restarts == 1);

    reset(); selected = 0; CHECK(kms_usb_recovery_request()); kms_usb_recovery_tick(true);
    now += KMS_USB_RECOVERY_DRAIN_MS; kms_usb_recovery_tick(true);
    reset_reason = ESP_RST_SW; kms_usb_recovery_init();
    CHECK(kms_usb_recovery_boot_slot(2) == 0 && kms_usb_recovery_boot_paused());
    reset_reason = ESP_RST_POWERON; kms_usb_recovery_init();
    CHECK(kms_usb_recovery_boot_slot(2) == 2 && guard.restarts == 0);
    guard.magic = 123; reset_reason = ESP_RST_SW; kms_usb_recovery_init();
    CHECK(guard.restarts == 0 && kms_usb_recovery_boot_slot(2) == 2);

    reset();
    for (unsigned n = 0; n < KMS_USB_RECOVERY_ATTEMPTS; ++n) {
        kms_usb_health_fault(KMS_USB_PROBE, 2, 0); kms_usb_recovery_tick(true);
        CHECK(kms_usb_recovery_active()); drain(); reconnect();
        CHECK(state == RUNNING);
    }
    kms_usb_health_fault(KMS_USB_PROBE, 2, 0); kms_usb_recovery_tick(true);
    CHECK(state == BLOCKED && off_calls == KMS_USB_RECOVERY_ATTEMPTS && !restarts);
    CHECK(kms_usb_recovery_request()); kms_usb_recovery_tick(true); drain(); reconnect();
    CHECK(state == RUNNING && attempts == 1);
    guard.restarts = 1; seal_guard();
    now += KMS_USB_RECOVERY_WINDOW_MS; kms_usb_recovery_tick(true);
    CHECK(attempts == 0 && guard.restarts == 0); // Healthy interval replenishes bounded recovery.

    reset(); CHECK(kms_usb_recovery_request()); kms_usb_recovery_tick(true); drain();
    healthy_inputs = 0; now += KMS_USB_RECOVERY_ENUM_MS; kms_usb_recovery_tick(true);
    CHECK(state == RUNNING && !restarts && off_calls == 1); // Hub physically absent: wait, do not reboot.
    reset(); power_error = true; CHECK(kms_usb_recovery_request()); kms_usb_recovery_tick(true);
    CHECK(state == RESTARTING && guard.restarts == 1);
    reset(); CHECK(kms_usb_recovery_request()); kms_usb_recovery_tick(true);
    power_error = true; drain(); CHECK(state == RESTARTING && guard.restarts == 1);
    reset(); CHECK(kms_usb_recovery_request()); kms_usb_recovery_tick(true); drain();
    healthy_inputs = 0; host_devices = 3; now += KMS_USB_RECOVERY_ENUM_MS; kms_usb_recovery_tick(true);
    CHECK(state == DRAINING && attempts == 2); // Partial re-enumeration has a deadline.
    CHECK(fault_captures == 1); // Keep the initial evidence across retries in the same outage.

    reset(); block_quiesce = true; CHECK(kms_usb_recovery_request());
    kms_usb_recovery_watch_begin(true); kms_usb_recovery_tick(true);
    CHECK(restarted_inside_call && restarts == 1 && guard.pending == 2 && guard.slot == 3);
    CHECK(last_event_phase == KMS_USB_HOST_RESET); // Reconnect history is queued before cleanup starts.
    CHECK(guard.fault.boot == 1234 && guard.fault.uptime_ms == KMS_USB_CALL_TIMEOUT_MS);
    CHECK(guard.fault.idle_ms == 5000 + KMS_USB_CALL_TIMEOUT_MS);
    CHECK(((guard.fault.detail >> 8) & 255u) == QUIESCING);
    reset_reason = ESP_RST_SW; kms_usb_recovery_init();
    CHECK(recovered_faults == 1 && recovered_fault.boot == 1234 && recovered_fault.slot == 3);
    CHECK(kms_usb_recovery_boot_slot(2) == 3 && guard.restarts == 1);
    kms_usb_recovery_init(); CHECK(recovered_faults == 1); // Import the RTC event only once.
    kms_usb_recovery_watch_begin(true); now += KMS_USB_CALL_TIMEOUT_MS; timer_config.callback(NULL);
    CHECK(restarts == 1); // Same warm-boot restart allowance, no reboot loop.

    reset(); CHECK(kms_usb_recovery_request()); kms_usb_recovery_tick(true);
    now += KMS_USB_RECOVERY_DRAIN_MS; kms_usb_recovery_tick(true);
    CHECK(state == RESTARTING && guard.pending == 1);
    kms_usb_recovery_watch_begin(true); blocked_call();
    CHECK(restarted_inside_call && guard.pending == 2); // A reserved fallback must still run if the next USB tick blocks.

    reset(); block_power = true; CHECK(kms_usb_recovery_request());
    kms_usb_recovery_watch_begin(true); kms_usb_recovery_tick(true);
    CHECK(restarted_inside_call && ((guard.fault.detail >> 8) & 255u) == DISCONNECTING);
    reset(); selected = 0; kms_usb_recovery_watch_begin(true); blocked_call();
    reset_reason = ESP_RST_SW; kms_usb_recovery_init();
    CHECK(kms_usb_recovery_boot_paused() && recovered_fault.slot == 0);
    reset(); kms_usb_recovery_watch_begin(false); now += KMS_USB_CALL_TIMEOUT_MS; timer_config.callback(NULL);
    CHECK(restarts == 0); // Updates/pairing/unconfirmed OTA boots cannot arm the call guard.
    reset(); kms_usb_recovery_watch_begin(true); kms_usb_recovery_watch_end();
    now += KMS_USB_CALL_TIMEOUT_MS; timer_config.callback(NULL); CHECK(restarts == 0);
    reset(); now = 0xfffffff0u; kms_usb_recovery_watch_begin(true);
    now += KMS_USB_CALL_TIMEOUT_MS - 1; timer_config.callback(NULL); CHECK(restarts == 0);
    ++now; timer_config.callback(NULL); CHECK(restarts == 1 && guard.fault.uptime_ms == now);
    reset_reason = ESP_RST_POWERON; kms_usb_recovery_init();
    CHECK(recovered_faults == 0 && guard.restarts == 0); // Power loss/corrupt RTC must not invent an event.

    reset(); CHECK(!kms_usb_recovery_test_block() && !manual && !test_block);
    test_mode = true; kms_usb_recovery_tick(false);
    CHECK(!kms_usb_recovery_test_block()); // OTA/pairing exclusions still apply in test mode.
    kms_usb_recovery_tick(true); CHECK(kms_usb_recovery_test_block());
    test_mode = false; kms_usb_recovery_watch_begin(true); kms_usb_recovery_tick(true);
    CHECK(!restarted_inside_call && !restarts && !test_block); // Leaving test mode cancels a queued freeze.
    reset(); test_mode = true; CHECK(kms_usb_recovery_test_block());
    kms_usb_recovery_watch_begin(true); kms_usb_recovery_tick(true);
    CHECK(restarted_inside_call && restarts == 1 && guard.pending == 2);
    CHECK(!kms_usb_recovery_test_block()); // Forced freezes share the restart allowance.
    return 0;
}
