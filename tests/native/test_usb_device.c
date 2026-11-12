// Exercise the production receiver USB queue/serializer with a fake TinyUSB host.
#include "kms_jiggler.h"
#include "tinyusb.h"
#include <string.h>
#include <stddef.h>
#include "kms_desktop_runtime.h"
void kms_desktop_usb_reset(void) { }
static bool power_awake = true;
bool kms_desktop_jiggler_allowed(void) { return power_awake; }
void kms_desktop_usb_set(const uint8_t *data, size_t n) { (void)data; (void)n; }
size_t kms_desktop_usb_get(unsigned report, uint8_t *data, size_t n) { (void)report; (void)data; (void)n; return 0; }
void kms_stream_usb_set(const uint8_t *data, size_t n) { (void)data; (void)n; }
size_t kms_stream_radio_status(uint8_t *data, size_t n) { (void)data; (void)n; return 0; }
void kms_clipboard_usb_set(const uint8_t *data, size_t n) { (void)data; (void)n; }
size_t kms_clipboard_usb_get(uint8_t *data, size_t n) { (void)data; (void)n; return 0; }
static uint32_t clock_ms;
static kms_jiggler_t jiggler;
static bool host_mounted, suspended, endpoint_ready, reject_report;
static unsigned wake_attempts;
static bool wake_allowed;
static bool text_enabled, text_shifted;
static bool callback_context, complete_during_submit;
static unsigned forbidden_callback_work;
static unsigned text_stage;
bool kms_link_text_active(void) { return text_enabled && text_stage < 2; }
bool kms_link_text_next(kms_sample_t *s)
{
    if (callback_context) ++forbidden_callback_work;
    if (!kms_link_text_active()) return false;
    *s = (kms_sample_t){0};
    if (!text_stage) { kms_set_key(&s->state, 4, true); s->state.modifiers = text_shifted ? 2 : 0; }
    return true;
}
void kms_link_text_delivered(void) { if (callback_context) ++forbidden_callback_work; ++text_stage; }
static uint8_t protocol = HID_PROTOCOL_REPORT, mouse[8];
static uint8_t keyboard[KMS_KEY_BYTES + 2];
static unsigned reports[3], mouse_size, checks, faults;
static unsigned wakeups;
static unsigned in_flight;
static uint8_t transfer_data[3][KMS_KEY_BYTES + 2];
static unsigned transfer_size[3];
static bool record_batch;
static unsigned batch_keys_count;
static uint8_t batch_keys[8];
static int32_t batch_motion;
void kms_app_wake(void) { ++wakeups; }
static uint32_t endpoint_until[3], transfer_ms, mouse_at;
static bool record_media, media_motion;
static unsigned media_keys, media_mouse, media_controls, media_errors;
uint32_t kms_now(void) { return clock_ms; }
bool kms_link_jiggler_motion(kms_jiggler_motion_t *out)
{ if (callback_context) ++forbidden_callback_work; return kms_jiggler_motion(&jiggler, clock_ms, out); }
void kms_link_jiggler_moved(void)
{ if (callback_context) ++forbidden_callback_work; kms_jiggler_moved(&jiggler, clock_ms); }
void kms_link_jiggler_activity(void)
{ if (callback_context) ++forbidden_callback_work; kms_jiggler_activity(&jiggler, clock_ms); }
bool tud_mounted(void) { return host_mounted; }
bool tud_suspended(void) { return suspended; }
bool tud_remote_wakeup(void) { ++wake_attempts; return wake_allowed; }
uint8_t tud_hid_n_get_protocol(unsigned instance) { (void)instance; return protocol; }
bool tud_hid_n_ready(unsigned instance)
{ return endpoint_ready && kms_time_elapsed(clock_ms, endpoint_until[instance], 0); }
bool tud_hid_n_report(unsigned instance, unsigned id, const void *data, unsigned size)
{
    if (reject_report) return false;
    if (record_media) {
        const uint8_t *bytes = data;
        if (instance == 0) {
            unsigned sequence = ++media_keys;
            if (!tud_hid_n_ready(1) || !tud_hid_n_ready(2) ||
                size != KMS_KEY_BYTES + 2 || ((unsigned)bytes[2] | (unsigned)bytes[3] << 8) != sequence ||
                bytes[0] != (sequence & 1 ? KMS_HID_CTRL_LEFT : 0)) ++media_errors;
        } else if (instance == 1) {
            unsigned sequence = ++media_mouse;
            if (!tud_hid_n_ready(0) || media_keys != sequence || size != 8 ||
                bytes[0] != (sequence & 1) || bytes[2] != (unsigned)media_motion || bytes[3] || bytes[4] || bytes[5]) ++media_errors;
        } else {
            unsigned sequence = ++media_controls;
            if (id != 1 || size != 8 || ((unsigned)bytes[0] | (unsigned)bytes[1] << 8) != 0x100 + sequence % 64) ++media_errors;
        }
    }
    ++reports[instance];
    endpoint_until[instance] = clock_ms + transfer_ms;
    in_flight |= 1u << instance;
    memcpy(transfer_data[instance], data, size); transfer_size[instance] = size;
    if (instance == 0) {
        memcpy(keyboard, data, size);
        if (record_batch && batch_keys_count < sizeof(batch_keys)) batch_keys[batch_keys_count++] = keyboard[2];
    }
    if (instance == 1) {
        memcpy(mouse, data, size); mouse_size = size; mouse_at = clock_ms;
        if (record_batch && size == 8) batch_motion += (int16_t)(mouse[2] | (uint16_t)mouse[3] << 8);
    }
    if (complete_during_submit) {
        in_flight &= ~(1u << instance);
        callback_context = true;
        tud_hid_report_complete_cb(instance, transfer_data[instance], transfer_size[instance]);
        callback_context = false;
    }
    return true;
}
int tinyusb_driver_install(const tinyusb_config_t *c) { (void)c; return 0; }
int esp_read_mac(uint8_t *mac, int type) { (void)type; memset(mac, 0, 6); return 0; }
int snprintf(char *out, size_t size, const char *format, ...)
{ (void)format; if (size) *out = 0; return 0; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void kms_fake_check(int result) { if (result) ++faults; }
void kms_slave_diagnostics_event(unsigned event, uint32_t detail) { (void)event; (void)detail; }
void kms_slave_diagnostics_add(unsigned metric, uint32_t value) { (void)metric; (void)value; }
void kms_slave_diagnostics_peak(unsigned metric, uint32_t value) { (void)metric; (void)value; }
void kms_slave_diagnostics_set(unsigned metric, uint32_t value) { (void)metric; (void)value; }
#include "../../firmware/receiver/main/usb_device.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static void drain(void) { for (unsigned i = 0; i < 24; ++i) kms_usb_device_tick(); }
static void reset_reports(void) { memset(reports, 0, sizeof(reports)); }
static void complete_transfers(void)
{
    for (unsigned instance = 0; instance < 3; ++instance) {
        if ((in_flight & (1u << instance)) && tud_hid_n_ready(instance)) {
            in_flight &= ~(1u << instance);
            callback_context = true;
            tud_hid_report_complete_cb(instance, transfer_data[instance], transfer_size[instance]);
            callback_context = false;
        }
    }
}
static void configure(bool enabled, uint16_t seconds)
{
    kms_packet_t command = {.type = KMS_JIGGLER, .session = 42, .sequence = jiggler.sequence + 1,
        .flags = enabled ? KMS_FLAG_JIGGLER_ENABLED : 0, .motion = {seconds}};
    (void)kms_jiggler_configure(&jiggler, &command, clock_ms);
}
static int media_throughput(unsigned batch_size, unsigned application_ms, bool motion, unsigned sample_ms)
{
    clock_ms = 100000; transfer_ms = 0;
    memset(endpoint_until, 0, sizeof(endpoint_until));
    kms_usb_device_clear(); drain(); reset_reports();
    in_flight = 0;
    media_keys = media_mouse = media_controls = media_errors = 0;
    record_media = true; media_motion = motion; transfer_ms = 1;
    unsigned offered = 0, peak_queue = 0;
    unsigned expected = 2000 / (sample_ms * batch_size) * batch_size;
    for (unsigned elapsed = 0; elapsed < 2200; ++elapsed, ++clock_ms) {
        complete_transfers();
        if (elapsed < expected * sample_ms && elapsed % (sample_ms * batch_size) == 0) {
            kms_sample_t samples[KMS_INPUT_BATCH_MAX] = {0};
            for (unsigned n = 0; n < batch_size; ++n) {
                unsigned sequence = ++offered;
                samples[n].state.keys[0] = (uint8_t)sequence;
                samples[n].state.keys[1] = (uint8_t)(sequence >> 8);
                samples[n].state.modifiers = sequence & 1 ? KMS_HID_CTRL_LEFT : 0;
                samples[n].state.buttons = sequence & 1;
                samples[n].state.consumer[0] = (uint16_t)(0x100 + sequence % 64);
                samples[n].x = motion ? 1 : 0;
            }
            CHECK(kms_usb_device_submit_many(samples, batch_size));
        }
        // USB callbacks preempt slower radio/application work. Input and host
        // completions keep their own clocks while application service is delayed.
        if (elapsed % application_ms == 0) kms_usb_device_tick();
        if (count > peak_queue) peak_queue = count;
        CHECK(!kms_usb_device_take_fault());
    }
    CHECK(offered == expected && media_keys == offered && media_mouse == offered && media_controls == offered);
    CHECK(!media_errors && !count && !active);
    CHECK(peak_queue <= batch_size);
    record_media = false; transfer_ms = 0;
    return 0;
}
__declspec(dllexport) int kms_run_usb_device_tests(void)
{
    CHECK(configuration[0] & TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP);
    kms_usb_device_init();
    host_mounted = endpoint_ready = true; clock_ms = 100; drain(); reset_reports();
    kms_sample_t held = {0}; held.state.modifiers = KMS_HID_CTRL_LEFT; held.state.buttons = 3;
    held.state.consumer[0] = 0xe9; held.state.system = 1; kms_set_key(&held.state, 4, true);
    unsigned wakes_before = wakeups;
    CHECK(kms_usb_device_submit(&held)); CHECK(wakeups == wakes_before + 1);
    tud_hid_report_complete_cb(0, keyboard, sizeof(keyboard)); CHECK(wakeups == wakes_before + 2);
    drain(); reset_reports();
    configure(true, 1); clock_ms = 1099; drain(); CHECK(!reports[1]);
    clock_ms = 1100; drain();
    CHECK(reports[1] == 1 && !reports[0] && !reports[2]);
    CHECK(mouse_size == 8 && mouse[0] == 3 && mouse[1] == 0 && mouse[2] == 255 && mouse[3] == 255);
    CHECK(kms_state_equal(&host_state, &held.state));
    reset_reports(); clock_ms = 2100; endpoint_ready = false; kms_usb_device_tick();
    CHECK(!reports[1] && jiggler.right && jiggler.changed == 1100);
    endpoint_ready = true; reject_report = true; kms_usb_device_tick();
    CHECK(!reports[1] && jiggler.right && jiggler.changed == 1100);
    reject_report = false; drain();
    CHECK(reports[1] == 1 && mouse[2] == 1 && mouse[3] == 0 && mouse[0] == 3);
    CHECK(kms_state_equal(&host_state, &held.state) && !reports[0] && !reports[2]);
    // Real movement postpones a due jiggle without losing motion or held state.
    reset_reports(); clock_ms = 3100; held.x = 10;
    CHECK(kms_usb_device_submit(&held)); drain();
    CHECK(reports[1] == 1 && mouse[2] == 10 && mouse[3] == 0 && mouse[0] == 3);
    CHECK(jiggler.changed == 3100 && !jiggler.right);
    CHECK(!pending.x && kms_state_equal(&host_state, &held.state));
    // The boot mouse format retains buttons and uses signed one-byte motion.
    configure(false, 1); protocol = HID_PROTOCOL_BOOT; drain();
    held.x = 0; CHECK(kms_usb_device_submit(&held)); drain(); reset_reports();
    configure(true, 1); clock_ms = 4100; drain();
    CHECK(reports[1] == 1 && mouse_size == 3 && mouse[0] == 3 && mouse[1] == 255 && mouse[2] == 0);
    // Real motion at the signed report limit remains a separate report.
    reset_reports(); clock_ms = 5100; held.x = 127;
    CHECK(kms_usb_device_submit(&held)); drain();
    CHECK(reports[1] == 1 && mouse[1] == 127 && jiggler.changed == 5100);
    clock_ms = 6099; drain(); CHECK(reports[1] == 1);
    clock_ms = 6100; drain(); CHECK(reports[1] == 2 && mouse[1] == 1);
    // OS notification pauses jiggles before USB suspension, including mixed reports.
    reset_reports(); power_awake = false; clock_ms = 15000; drain();
    CHECK(!reports[1] && jiggler.enabled && !wake_attempts);
    held.x = 4; CHECK(kms_usb_device_submit(&held)); drain();
    CHECK(reports[1] == 1 && mouse[1] == 4);
    held.x = 0; power_awake = true; drain();
    CHECK(reports[1] == 1 && jiggler.changed == 15000);
    // Suspension/unmount keeps the timer enabled and produces no catch-up burst.
    reset_reports(); suspended = true; clock_ms = 20000; drain();
    CHECK(!reports[1] && jiggler.enabled && jiggler.changed == 15000);
    CHECK(kms_usb_device_ready() && !wake_attempts);
    kms_sample_t neutral = {0};
    held.x = 0; CHECK(kms_usb_device_submit(&held) && !wake_attempts); // Repeated held snapshot.
    CHECK(kms_usb_device_submit(&neutral) && !wake_attempts);
    // Host permission is respected, and a denied request can be retried.
    held.x = 0; CHECK(kms_usb_device_submit(&held)); CHECK(wake_attempts == 1);
    wake_allowed = true;
    held.x = 1; CHECK(kms_usb_device_submit(&held)); CHECK(wake_attempts == 2);
    CHECK(kms_usb_device_submit(&held)); CHECK(wake_attempts == 2 && !count);
    suspended = false; drain(); CHECK(jiggler.changed == 20000);
    CHECK(kms_state_equal(&host_state, &neutral.state));
    // Mouse-only input wakes a later sleep; neutral traffic and jiggles do not.
    suspended = true; drain();
    CHECK(kms_usb_device_submit(&neutral) && wake_attempts == 2);
    neutral.x = 1; CHECK(kms_usb_device_submit(&neutral)); CHECK(wake_attempts == 3);
    host_mounted = false; drain(); CHECK(!kms_usb_device_ready());
    CHECK(kms_usb_device_submit(&neutral) && wake_attempts == 3);
    suspended = false; host_mounted = true; drain();
    unsigned after_resume = reports[1]; drain(); CHECK(reports[1] == after_resume);
    configure(false, 1); reset_reports(); clock_ms = 50000; drain(); CHECK(!reports[1]);
    // Ten seconds of real input with asynchronous endpoints must not wedge the
    // serializer. Then ordinary 200ms snapshots must allow repeated idle jiggles.
    protocol = HID_PROTOCOL_REPORT; drain();
    transfer_ms = 8; configure(true, 1);
    for (; clock_ms < 60000; clock_ms += 2) {
        if (clock_ms % 12 == 0) {
            held.x = 4; held.y = -2; held.wheel = 1; held.pan = -1;
            CHECK(kms_usb_device_submit(&held));
        }
        kms_usb_device_tick();
        CHECK(!kms_usb_device_take_fault());
    }
    held.x = held.y = held.wheel = held.pan = 0;
    // Drain the last real report before measuring the idle deadline.
    for (unsigned i = 0; i < 20; ++i, clock_ms += 2) kms_usb_device_tick();
    uint32_t last_real = jiggler.changed;
    reset_reports();
    for (; clock_ms < last_real + 4000; clock_ms += 2) {
        if (clock_ms % KMS_SNAPSHOT_MS == 0) {
            held.state.modifiers ^= KMS_HID_CTRL_LEFT;
            CHECK(kms_usb_device_submit(&held));
        }
        unsigned before = reports[1]; kms_usb_device_tick();
        CHECK(!kms_usb_device_take_fault());
        if (reports[1] != before) {
            CHECK(mouse_at >= last_real + reports[1] * 1000 && mouse_at <= last_real + reports[1] * 1000 + 20);
            CHECK(mouse[2] == (reports[1] % 2 ? 255 : 1) && mouse[0] == held.state.buttons);
        }
    }
    CHECK(reports[1] == 3 && kms_state_equal(&host_state, &held.state));
    // Activity arriving while a jiggle is waiting on USB cancels that due move.
    endpoint_ready = false;
    clock_ms = last_real + 4000; kms_usb_device_tick();
    held.y = 2; CHECK(kms_usb_device_submit(&held)); held.y = 0;
    endpoint_ready = true;
    for (unsigned i = 0; i < 20; ++i, clock_ms += 2) kms_usb_device_tick();
    CHECK(reports[1] == 4 && mouse[2] == 0 && mouse[4] == 2);
    last_real = jiggler.changed;
    for (; clock_ms < last_real + 1000; clock_ms += 2) kms_usb_device_tick();
    CHECK(reports[1] == 4);
    for (unsigned i = 0; i < 20; ++i, clock_ms += 2) kms_usb_device_tick();
    CHECK(reports[1] == 5 && mouse[2] == 1 && jiggler.enabled);
    // Configured maximum distance fits both HID formats and retains held buttons.
    configure(false, 1); transfer_ms = 0; memset(endpoint_until, 0, sizeof(endpoint_until));
    protocol = HID_PROTOCOL_BOOT; drain(); CHECK(kms_usb_device_submit(&held)); drain(); reset_reports();
    kms_packet_t distance_command = {.type = KMS_JIGGLER, .session = 42, .sequence = jiggler.sequence + 1,
        .flags = KMS_FLAG_JIGGLER_ENABLED, .motion = {1, 0, 0, 127}};
    CHECK(kms_jiggler_configure(&jiggler, &distance_command, clock_ms));
    clock_ms += 1000; drain(); CHECK(reports[1] == 1 && (int8_t)mouse[1] == -127 && mouse[0] == held.state.buttons);
    clock_ms += 1000; drain(); CHECK(reports[1] == 2 && (int8_t)mouse[1] == 127);
    protocol = HID_PROTOCOL_REPORT; drain(); reset_reports();
    clock_ms += 1000; drain(); CHECK(reports[1] == 1 && (int16_t)(mouse[2] | mouse[3] << 8) == -127);
    CHECK(kms_usb_device_submit(&held)); drain(); reset_reports();
    distance_command.sequence = jiggler.sequence + 1;
    distance_command.flags = KMS_FLAG_JIGGLER_ENABLED | (KMS_JIGGLER_FOUR_WAY << KMS_JIGGLER_MODE_SHIFT);
    distance_command.motion[3] = 3;
    CHECK(kms_jiggler_configure(&jiggler, &distance_command, clock_ms));
    static const int16_t directions[4][2] = {{0,-3}, {0,3}, {3,0}, {-3,0}};
    for (unsigned step = 0; step < 8; ++step) {
        clock_ms += 1000; drain();
        CHECK(reports[1] == step + 1);
        CHECK((int16_t)(mouse[2] | mouse[3] << 8) == directions[step % 4][0]);
        CHECK((int16_t)(mouse[4] | mouse[5] << 8) == directions[step % 4][1]);
        CHECK(mouse[0] == held.state.buttons && kms_state_equal(&host_state, &held.state));
    }
    protocol = HID_PROTOCOL_BOOT; drain(); reset_reports();
    clock_ms += 1000; drain(); CHECK(reports[1] == 1 && mouse_size == 3 && !mouse[1] && (int8_t)mouse[2] == -3);
    protocol = HID_PROTOCOL_REPORT; drain(); reset_reports();
    CHECK(kms_usb_device_submit(&held)); drain(); reset_reports();
    distance_command.sequence = jiggler.sequence + 1;
    distance_command.flags = KMS_FLAG_JIGGLER_ENABLED | (KMS_JIGGLER_RANDOM << KMS_JIGGLER_MODE_SHIFT);
    CHECK(kms_jiggler_configure(&jiggler, &distance_command, clock_ms));
    uint32_t random_start = clock_ms; transfer_ms = 8;
    for (; clock_ms < random_start + 3000; clock_ms += 2) {
        if (clock_ms % KMS_SNAPSHOT_MS == 0) CHECK(kms_usb_device_submit(&held));
        kms_usb_device_tick(); CHECK(!kms_usb_device_take_fault());
        if (clock_ms == random_start + 998) CHECK(!reports[1]);
    }
    CHECK(reports[1] >= 95 && reports[1] <= 100 && jiggler.mode == KMS_JIGGLER_RANDOM);
    held.x = 10; CHECK(kms_usb_device_submit(&held)); held.x = 0;
    for (unsigned i = 0; i < 20; ++i, clock_ms += 2) kms_usb_device_tick();
    uint32_t resume_at = jiggler.changed + 1000; reset_reports();
    for (; clock_ms < resume_at; clock_ms += 2) kms_usb_device_tick();
    CHECK(!reports[1] && jiggler.enabled);
    for (unsigned i = 0; i < 5; ++i, clock_ms += 2) kms_usb_device_tick();
    CHECK(reports[1] == 1 && jiggler.running);
    CHECK(!kms_usb_device_take_fault() && !faults);
    // A receiver-local character waits for each USB completion, including the
    // final key-up. No radio acknowledgement occurs merely on queue/submission.
    configure(false, 1); transfer_ms = 0; drain();
    CHECK(kms_usb_device_submit(&neutral)); drain(); reset_reports();
    memset(endpoint_until, 0, sizeof(endpoint_until)); transfer_ms = 8;
    text_enabled = true; text_stage = 0;
    kms_usb_device_tick(); CHECK(reports[0] == 1 && text_stage == 0);
    clock_ms += 7; kms_usb_device_tick(); CHECK(text_stage == 0 && reports[0] == 1);
    ++clock_ms; kms_usb_device_tick(); CHECK(text_stage == 1 && reports[0] == 2);
    clock_ms += 7; kms_usb_device_tick(); CHECK(text_stage == 1);
    ++clock_ms; kms_usb_device_tick(); CHECK(text_stage == 2 && !kms_link_text_active());
    CHECK(kms_state_equal(&host_state, &neutral.state));
    // A rejected endpoint write retains the current text position for retry.
    text_stage = 0; reject_report = true; kms_usb_device_tick(); CHECK(text_stage == 0);
    reject_report = false; kms_usb_device_tick(); CHECK(text_stage == 0);
    clock_ms += 8; kms_usb_device_tick(); CHECK(text_stage == 1);
    clock_ms += 8; kms_usb_device_tick(); CHECK(text_stage == 2);
    // Even an instantly ready endpoint must retain the five-millisecond gap
    // for Shift+key-down, final key-up, and the start of the next text batch.
    transfer_ms = 0; memset(endpoint_until, 0, sizeof(endpoint_until));
    clock_ms += 5; text_stage = 0; text_shifted = true; reset_reports();
    kms_usb_device_tick(); CHECK(reports[0] == 1 && text_stage == 0 && keyboard[0] == 2);
    kms_usb_device_tick(); CHECK(reports[0] == 1 && text_stage == 1);
    clock_ms += 4; kms_usb_device_tick(); CHECK(reports[0] == 1 && text_stage == 1);
    ++clock_ms; kms_usb_device_tick(); CHECK(reports[0] == 2 && text_stage == 1 && !keyboard[0]);
    kms_usb_device_tick(); CHECK(text_stage == 2 && kms_state_equal(&host_state, &neutral.state));
    text_stage = 0; kms_usb_device_tick(); CHECK(reports[0] == 2);
    clock_ms += 5; kms_usb_device_tick(); CHECK(reports[0] == 3 && text_stage == 0);
    kms_usb_device_tick(); clock_ms += 5; kms_usb_device_tick(); kms_usb_device_tick();
    CHECK(reports[0] == 4 && text_stage == 2);
    // Ordinary keyboard input bypasses the synthetic typing throttle.
    text_enabled = false;
    kms_sample_t ordinary = {0}; kms_set_key(&ordinary.state, 5, true);
    CHECK(kms_usb_device_submit(&ordinary)); kms_usb_device_tick();
    CHECK(reports[0] == 5 && !keyboard[0] && (keyboard[2] & (1u << 5)));
    drain();
    // The short pacing interval works across the 32-bit millisecond wrap.
    text_enabled = true; text_stage = 0; clock_ms = 0xfffffffdu;
    for (unsigned instance = 0; instance < 3; ++instance) endpoint_until[instance] = clock_ms;
    kms_usb_device_tick(); kms_usb_device_tick(); CHECK(reports[0] == 6 && text_stage == 1);
    clock_ms = 1; kms_usb_device_tick(); CHECK(reports[0] == 6);
    clock_ms = 2; kms_usb_device_tick(); CHECK(reports[0] == 7 && text_stage == 1);
    kms_usb_device_tick(); CHECK(text_stage == 2 && !keyboard[0]);
    text_enabled = false;
    // Ordered radio batches retain each key transition and relative movement.
    transfer_ms = 0; kms_usb_device_clear(); drain(); reset_reports();
    configure(false, 1); protocol = HID_PROTOCOL_REPORT;
    kms_sample_t batch[4] = {{.x = 1}, {.x = 2}, {.x = 3}, {.x = 4}};
    kms_set_key(&batch[0].state, 4, true); kms_set_key(&batch[1].state, 5, true);
    kms_set_key(&batch[2].state, 6, true);
    record_batch = true; batch_keys_count = 0; batch_motion = 0;
    CHECK(kms_usb_device_submit_many(batch, 4)); drain();
    CHECK(batch_keys_count == 4 && batch_keys[0] == 0x10 && batch_keys[1] == 0x20 &&
        batch_keys[2] == 0x40 && !batch_keys[3]);
    CHECK(batch_motion == 10 && kms_state_equal(&host_state, &batch[3].state));
    CHECK(!kms_usb_device_take_fault());
    // Insufficient queue room rejects the entire batch and forces neutral.
    endpoint_ready = false;
    for (unsigned i = 0; i < KMS_USB_QUEUE_DEPTH - 1; ++i) CHECK(kms_usb_device_submit(&batch[0]));
    CHECK(!kms_usb_device_submit_many(batch, 4) && !count && force_neutral);
    CHECK(kms_usb_device_take_fault());
    batch_keys_count = 0; batch_motion = 0; endpoint_ready = true; drain();
    CHECK(batch_keys_count <= 1 && !batch_keys[0] && !batch_motion && !host_state.buttons);
    record_batch = false;
    CHECK(!kms_usb_device_submit_many(NULL, 1) && !kms_usb_device_submit_many(batch, 0));
    CHECK(!kms_usb_device_submit_many(batch, KMS_INPUT_BATCH_MAX + 1) && !count);
    int failure = media_throughput(1, 2, true, 5); if (failure) return failure;
    failure = media_throughput(4, 2, true, 5); if (failure) return failure;
    failure = media_throughput(4, 3, true, 5); if (failure) return failure;
    // Hardware traces include several milliseconds of application work between
    // samples. Completion must drain already-queued input without that delay.
    failure = media_throughput(4, 7, true, 5); if (failure) return failure;
    // Stationary mouse-button chords can continue without querying the jiggler
    // from USB context, and must retain every keyboard/button/media transition.
    failure = media_throughput(4, 7, false, 5); if (failure) return failure;
    // Larger radio batches must fit the existing USB queue and preserve every
    // chord at 333 samples/s, including while application work takes seven ms.
    failure = media_throughput(7, 7, true, 3); if (failure) return failure;
    failure = media_throughput(7, 7, false, 3); if (failure) return failure;
    // A slow media endpoint must fence the next sample's modifier release,
    // even after the parallel mouse transfer has completed.
    kms_usb_device_clear(); drain(); reset_reports(); transfer_ms = 1;
    held = (kms_sample_t){.x = 1}; held.state.modifiers = KMS_HID_CTRL_LEFT;
    held.state.consumer[0] = 0xe9; kms_set_key(&held.state, 4, true);
    CHECK(kms_usb_device_submit(&held)); kms_usb_device_tick();
    CHECK(reports[0] == 1 && !reports[1] && !reports[2]);
    ++clock_ms; kms_usb_device_tick();
    CHECK(reports[1] == 1 && reports[2] == 1);
    endpoint_until[2] = clock_ms + 8;
    neutral = (kms_sample_t){.x = 1};
    CHECK(kms_usb_device_submit(&neutral));
    clock_ms += 7; kms_usb_device_tick();
    CHECK(reports[0] == 1 && keyboard[0] == KMS_HID_CTRL_LEFT && count == 1);
    ++clock_ms; kms_usb_device_tick();
    CHECK(reports[0] == 2 && !keyboard[0] && !count);
    for (unsigned i = 0; i < 8; ++i, ++clock_ms) kms_usb_device_tick();
    CHECK(!active && !kms_usb_device_take_fault());
    // A truly blocked host still reaches the existing 200 ms safety deadline.
    transfer_ms = 0; endpoint_ready = false;
    CHECK(kms_usb_device_submit(&held)); kms_usb_device_tick();
    clock_ms += KMS_USB_MAX_AGE_MS - 1; kms_usb_device_tick();
    CHECK(!kms_usb_device_take_fault());
    ++clock_ms; kms_usb_device_tick();
    CHECK(kms_usb_device_take_fault() && !count);
    endpoint_ready = true; drain();
    CHECK(!active && !host_state.modifiers && !host_state.consumer[0]);
    // A late completion cannot bypass the stale-input deadline in the callback.
    reset_reports(); transfer_ms = 1; in_flight = 0;
    CHECK(kms_usb_device_submit(&held)); kms_usb_device_tick();
    clock_ms += KMS_USB_MAX_AGE_MS; complete_transfers();
    CHECK(reports[0] == 1 && !reports[1] && !reports[2]);
    kms_usb_device_tick(); CHECK(kms_usb_device_take_fault());
    for (unsigned i = 0; i < 12; ++i, ++clock_ms) { complete_transfers(); kms_usb_device_tick(); }
    CHECK(!active && !host_state.modifiers);
    // A callback cannot dequeue input that expired while the application was
    // busy, even if there is no currently active sample.
    transfer_ms = 0; in_flight = 0; reset_reports();
    CHECK(kms_usb_device_submit(&held));
    clock_ms += KMS_USB_MAX_AGE_MS;
    callback_context = true;
    tud_hid_report_complete_cb(0, keyboard, sizeof(keyboard));
    callback_context = false;
    CHECK(!reports[0] && !reports[1] && !reports[2] && count == 1 && !active);
    kms_usb_device_tick(); CHECK(kms_usb_device_take_fault() && !count);
    drain(); CHECK(!active && !host_state.modifiers);
    // Completion can preempt submission while the application owns the guard.
    // The USB task must skip continuation instead of blocking or reentering it.
    unsigned blocked_callbacks = kms_fake_nonblocking_rejections;
    complete_during_submit = true; transfer_ms = 0; reset_reports();
    CHECK(kms_usb_device_submit(&held)); drain();
    complete_during_submit = false;
    CHECK(kms_fake_nonblocking_rejections >= blocked_callbacks + 3 && !faults);
    CHECK(reports[0] == 1 && reports[1] == 1 && reports[2] == 1);
    CHECK(kms_state_equal(&host_state, &held.state) && !active && !forbidden_callback_work);
    return 0;
}
__declspec(dllexport) int kms_usb_device_test_count(void) { return checks; }
