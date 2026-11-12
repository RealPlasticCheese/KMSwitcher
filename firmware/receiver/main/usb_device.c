#include "usb_device.h"
#include "kms_runtime.h"
#include "kms_slave_diagnostics.h"
#include "kms_test.h"
#include "kms_desktop_runtime.h"
#include "kms_stream_radio.h"
#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#if defined(CFG_TUD_HID_EP_BUFSIZE)
_Static_assert(CFG_TUD_HID_EP_BUFSIZE >= KMS_C_BYTES + 1,
               "HID control buffer must hold a desktop feature report and its ID");
#endif

enum { ITF_KEYBOARD, ITF_MOUSE, ITF_CONTROLS, ITF_COUNT };
enum { REPORT_CONSUMER = 1, REPORT_SYSTEM = 2, BOOT_KEYS = 6, KEYBOARD_BYTES = KMS_KEY_BYTES + 2,
       MOUSE_BYTES = 8, BOOT_MOUSE_BYTES = 3, USB_BCD_VERSION = 0x0200, DEVICE_BCD_VERSION = 0x0103,
       TEXT_STEP_MS = 5 };
static const uint8_t keyboard_descriptor[] = {
    0x05,0x01,0x09,0x06,0xa1,0x01, // Generic Desktop / Keyboard
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x01, // reserved byte
    0x05,0x08,0x19,0x01,0x29,0x05,0x75,0x01,0x95,0x05,0x91,0x02,
    0x75,0x03,0x95,0x01,0x91,0x01, // keyboard LED output
    0x05,0x07,0x19,0x00,0x2a,0xff,0x00,0x75,0x01,0x96,0x00,0x01,0x81,0x02,0xc0
};
static const uint8_t mouse_descriptor[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x10,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x10,0x81,0x02,
    0x05,0x01,0x09,0x30,0x09,0x31,0x16,0x01,0x80,0x26,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x06,
    0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,
    0x05,0x0c,0x0a,0x38,0x02,0x81,0x06,0xc0,0xc0
};
static const uint8_t controls_descriptor[] = {
    0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,REPORT_CONSUMER,
    0x15,0x00,0x27,0xff,0xff,0x00,0x00,0x19,0x00,0x2a,0xff,0xff,
    0x75,0x10,0x95,KMS_CONSUMERS,0x81,0x00,0xc0,
    0x05,0x01,0x09,0x80,0xa1,0x01,0x85,REPORT_SYSTEM,
    0x19,0x81,0x29,0x83,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x03,0x81,0x02,
    0x75,0x05,0x95,0x01,0x81,0x01,0xc0
    // A separate vendor collection is accessible without opening keyboard/mouse collections.
    ,0x06,0x00,0xff,0x09,0x02,0xa1,0x01,
    0x85,KMS_D_COMMAND_REPORT,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,KMS_D_BYTES,0x09,0x02,0xb1,0x02,
    0x85,KMS_D_STATUS_REPORT,0x95,KMS_D_BYTES,0x09,0x03,0xb1,0x02,
    0x85,KMS_C_SEND_REPORT,0x95,KMS_C_BYTES,0x09,0x04,0xb1,0x02,
    0x85,KMS_C_RECEIVE_REPORT,0x95,KMS_C_BYTES,0x09,0x05,0xb1,0x02,
    0x85,KMS_STREAM_REPORT,0x95,32,0x09,0x06,0xb1,0x02,0xc0
#ifdef CONFIG_KMS_TEST_SUPPORT
    ,0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x85,KMS_TEST_FEATURE,
    0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,KMS_TEST_PAGE,0x09,0x01,0xb1,0x02,0xc0
#endif
};
static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD_VERSION, .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = KMS_USB_VID, .idProduct = KMS_USB_PID, .bcdDevice = DEVICE_BCD_VERSION,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1
};
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUD_CONFIG_DESC_LEN + ITF_COUNT * TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, KMS_USB_POWER_MA),
    TUD_HID_DESCRIPTOR(ITF_KEYBOARD, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(keyboard_descriptor), 0x81,
                       KMS_USB_ENDPOINT_BYTES, KMS_USB_ENDPOINT_INTERVAL_MS),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, 5, HID_ITF_PROTOCOL_MOUSE, sizeof(mouse_descriptor), 0x82,
                       KMS_USB_ENDPOINT_BYTES, KMS_USB_ENDPOINT_INTERVAL_MS),
    TUD_HID_DESCRIPTOR(ITF_CONTROLS, 6, HID_ITF_PROTOCOL_NONE, sizeof(controls_descriptor), 0x83,
                       KMS_USB_ENDPOINT_BYTES, KMS_USB_ENDPOINT_INTERVAL_MS)
};
static char serial[13];
static const char *strings[] = {"\x09\x04", KMS_USB_MANUFACTURER, KMS_USB_PRODUCT, serial,
                               "Keyboard", "Mouse", "Media and system keys"};
static kms_sample_t queue[KMS_USB_QUEUE_DEPTH], pending;
static uint32_t queued_at[KMS_USB_QUEUE_DEPTH], pending_at;
static unsigned head, count, phase, waiting_interfaces;
static bool active, was_ready, force_neutral, stalled;
static bool text_report;
static uint32_t text_sent_at;
static kms_state_t sent, host_state, input_state;
static kms_led_state_t keyboard_leds;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t output_guard_storage;
static SemaphoreHandle_t output_guard;
static bool deferred_motion_activity;
static void clear_output(void);
static void tick_output(bool completion_context);
static void lock_output(void)
{
    if (output_guard) {
        bool acquired = xSemaphoreTake(output_guard, portMAX_DELAY) == pdTRUE;
        configASSERT(acquired); (void)acquired;
    }
}
static void unlock_output(void)
{ if (output_guard) (void)xSemaphoreGive(output_guard); }
static uint8_t keyboard_protocol = HID_PROTOCOL_REPORT, mouse_protocol = HID_PROTOCOL_REPORT;
static bool previous_mount, previous_suspend, wake_requested;
static unsigned keyboard_report(const kms_state_t *s, uint8_t *out)
{
    memset(out, 0, KEYBOARD_BYTES); out[0] = s->modifiers;
    if (tud_hid_n_get_protocol(ITF_KEYBOARD) == HID_PROTOCOL_REPORT) {
        memcpy(out + 2, s->keys, KMS_KEY_BYTES); return KEYBOARD_BYTES;
    }
    unsigned n = 0;
    for (unsigned key = 4; key < 224; ++key) if (kms_key(s, key)) {
        if (n < BOOT_KEYS) out[2 + n] = key;
        ++n;
    }
    if (n > BOOT_KEYS) memset(out + 2, KMS_HID_ERROR_ROLLOVER, BOOT_KEYS);
    return BOOT_KEYS + 2;
}
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == ITF_KEYBOARD ? keyboard_descriptor : instance == ITF_MOUSE ? mouse_descriptor : controls_descriptor;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t *out, uint16_t capacity)
{
    if (instance == ITF_CONTROLS && type == HID_REPORT_TYPE_FEATURE && id == KMS_STREAM_REPORT)
        return (uint16_t)kms_stream_radio_status(out, capacity);
    if (instance == ITF_CONTROLS && type == HID_REPORT_TYPE_FEATURE && id == KMS_C_RECEIVE_REPORT)
        return (uint16_t)kms_clipboard_usb_get(out, capacity);
    if (instance == ITF_CONTROLS && type == HID_REPORT_TYPE_FEATURE &&
        (id == KMS_D_COMMAND_REPORT || id == KMS_D_STATUS_REPORT))
        return (uint16_t)kms_desktop_usb_get(id, out, capacity);
#ifdef CONFIG_KMS_TEST_SUPPORT
    if (instance == ITF_CONTROLS && id == KMS_TEST_FEATURE && type == HID_REPORT_TYPE_FEATURE)
        return (uint16_t)kms_test_page(out, capacity);
#endif
    if (instance == ITF_KEYBOARD && !id && type == HID_REPORT_TYPE_OUTPUT && capacity) {
        portENTER_CRITICAL(&state_lock); out[0] = keyboard_leds.leds; portEXIT_CRITICAL(&state_lock);
        return 1;
    }
    if (type != HID_REPORT_TYPE_INPUT) return 0;
    kms_state_t s;
    portENTER_CRITICAL(&state_lock); s = host_state; portEXIT_CRITICAL(&state_lock);
    uint8_t data[KEYBOARD_BYTES] = {0}; unsigned n = 0;
    if (instance == ITF_KEYBOARD && !id) n = keyboard_report(&s, data);
    else if (instance == ITF_MOUSE && !id) {
        data[0] = s.buttons; data[1] = s.buttons >> 8;
        n = tud_hid_n_get_protocol(instance) == HID_PROTOCOL_BOOT ? BOOT_MOUSE_BYTES : MOUSE_BYTES;
        if (n == BOOT_MOUSE_BYTES) data[1] = 0;
    } else if (instance == ITF_CONTROLS && id == REPORT_CONSUMER) {
        n = sizeof(s.consumer); memcpy(data, s.consumer, n);
    } else if (instance == ITF_CONTROLS && id == REPORT_SYSTEM) { n = 1; data[0] = s.system; }
    if (n > capacity) n = capacity;
    memcpy(out, data, n); return n;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, const uint8_t *data, uint16_t size)
{
    if (instance == ITF_CONTROLS && id == KMS_STREAM_REPORT && type == HID_REPORT_TYPE_FEATURE) {
        kms_stream_usb_set(data, size); return;
    }
    if (instance == ITF_CONTROLS && id == KMS_C_SEND_REPORT && type == HID_REPORT_TYPE_FEATURE) {
        kms_clipboard_usb_set(data, size); return;
    }
    if (instance == ITF_CONTROLS && id == KMS_D_COMMAND_REPORT && type == HID_REPORT_TYPE_FEATURE) {
        kms_desktop_usb_set(data, size); return;
    }
#ifdef CONFIG_KMS_TEST_SUPPORT
    if (instance == ITF_CONTROLS && id == KMS_TEST_FEATURE && type == HID_REPORT_TYPE_FEATURE) {
        kms_test_command(data, size); return;
    }
#endif
    if (instance != ITF_KEYBOARD || id || type != HID_REPORT_TYPE_OUTPUT || !data || size != 1) return;
    portENTER_CRITICAL(&state_lock);
    kms_led_state_set(&keyboard_leds, true, data[0]);
    portEXIT_CRITICAL(&state_lock);
}
// TinyUSB callbacks run on its USB task. Never send radio/USB host traffic here.
static void reset_keyboard_leds(void)
{
    portENTER_CRITICAL(&state_lock); kms_led_state_set(&keyboard_leds, false, 0); portEXIT_CRITICAL(&state_lock);
}
static void device_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED || event->id == TINYUSB_EVENT_DETACHED) {
        reset_keyboard_leds(); kms_desktop_usb_reset();
    }
}
void kms_usb_device_keyboard_leds(kms_led_state_t *out)
{ portENTER_CRITICAL(&state_lock); *out = keyboard_leds; portEXIT_CRITICAL(&state_lock); }
void kms_usb_device_init(void)
{
    output_guard = xSemaphoreCreateMutexStatic(&output_guard_storage);
    configASSERT(output_guard);
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    tinyusb_config_t c = TINYUSB_DEFAULT_CONFIG();
    c.event_cb = device_event;
    c.task.size = KMS_USB_TASK_STACK; c.task.priority = KMS_USB_TASK_PRIORITY;
    c.descriptor.device = &device_descriptor; c.descriptor.full_speed_config = configuration;
    c.descriptor.string = strings; c.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&c));
}
// A suspended host can still accept a remote-wake request through the link.
bool kms_usb_device_ready(void) { return tud_mounted(); }
bool kms_usb_device_suspended(void) { return tud_suspended(); }
void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t length)
{
    (void)instance; (void)report; (void)length;
    kms_test_record(KMS_T_USB_COMPLETE, instance, 0, 0, report, length);
    kms_slave_diagnostics_add(KMS_S_COMPLETED, 1);
    // Drain ordinary input already accepted by the application. Never block
    // the USB task behind it, and leave radio/text/jiggler work there.
    if (output_guard && xSemaphoreTake(output_guard, 0) == pdTRUE) {
        tick_output(true);
        (void)xSemaphoreGive(output_guard);
    }
    kms_app_wake();
}
static bool wake_activity(const kms_sample_t *s)
{
    bool activity = s->x || s->y || s->wheel || s->pan ||
        (s->state.modifiers & ~input_state.modifiers) ||
        (s->state.buttons & ~input_state.buttons) ||
        (s->state.system & ~input_state.system);
    for (unsigned i = 0; i < KMS_KEY_BYTES; ++i)
        activity |= (s->state.keys[i] & ~input_state.keys[i]) != 0;
    for (unsigned i = 0; i < KMS_CONSUMERS; ++i) {
        if (!s->state.consumer[i]) continue;
        bool held = false;
        for (unsigned j = 0; j < KMS_CONSUMERS; ++j)
            held |= s->state.consumer[i] == input_state.consumer[j];
        activity |= !held;
    }
    input_state = s->state;
    return activity;
}
static bool submit_sample(const kms_sample_t *s)
{
    kms_test_sample(KMS_T_USB_QUEUE, count, s);
    kms_state_t zero = {0};
    bool activity = wake_activity(s);
    if (tud_suspended()) {
        // Only fresh routed input wakes the PC. Activation/release and the local
        // jiggler must not wake it. TinyUSB checks the host's wake permission.
        if (!wake_requested && tud_mounted() && activity)
            wake_requested = tud_remote_wakeup();
        // Do not replay activity accumulated while the host was asleep.
        return true;
    }
    // A neutral release has priority over a stalled host's queued input.
    if (kms_state_equal(&s->state, &zero) && !s->x && !s->y && !s->wheel && !s->pan) {
        // Preserve ordinary key-up ordering; only overflow invokes the force path.
        if (count == KMS_USB_QUEUE_DEPTH) {
            kms_slave_diagnostics_event(KMS_S_USB_OVERFLOW, count);
            clear_output(); stalled = true; return false;
        }
    }
    if (count == KMS_USB_QUEUE_DEPTH) {
        kms_slave_diagnostics_event(KMS_S_USB_OVERFLOW, count);
        kms_test_record(KMS_T_FAULT, KMS_S_USB_OVERFLOW, count, 0, NULL, 0);
        clear_output(); stalled = true;
        ESP_LOGW("usb-device", "Output queue full; clearing held input"); return false;
    }
    unsigned index = (head + count++) % KMS_USB_QUEUE_DEPTH;
    queue[index] = *s; queued_at[index] = kms_now();
    if (s->x || s->y || s->wheel || s->pan) kms_link_jiggler_activity();
    kms_slave_diagnostics_peak(KMS_S_PEAK_USB, count);
    kms_app_wake();
    return true;
}
static int32_t chunk(int32_t n, int32_t limit) { return n > limit ? limit : n < -limit ? -limit : n; }
static bool submit_samples(const kms_sample_t *samples, unsigned length)
{
    if (!samples || !length || length > KMS_INPUT_BATCH_MAX) return false;
    // The output guard excludes completion callbacks while reserving a batch,
    // so a retry cannot duplicate motion by accepting only part of the batch.
    if (!tud_suspended() && length > KMS_USB_QUEUE_DEPTH - count) {
        kms_slave_diagnostics_event(KMS_S_USB_OVERFLOW, count);
        kms_test_record(KMS_T_FAULT, KMS_S_USB_OVERFLOW, count, 0, NULL, 0);
        clear_output(); stalled = true; return false;
    }
    for (unsigned i = 0; i < length; ++i) if (!submit_sample(&samples[i])) return false;
    return true;
}
static void clear_output(void)
{
    kms_test_record(KMS_T_CLEAR, count, 0, 0, NULL, 0);
    kms_slave_diagnostics_event(KMS_S_OUTPUT_CLEAR, count);
    head = count = 0; active = text_report = false; force_neutral = true;
    memset(&input_state, 0, sizeof(input_state));
}
bool kms_usb_device_submit(const kms_sample_t *sample)
{ lock_output(); bool ok = submit_sample(sample); unlock_output(); return ok; }
bool kms_usb_device_submit_many(const kms_sample_t *samples, unsigned length)
{ lock_output(); bool ok = submit_samples(samples, length); unlock_output(); return ok; }
void kms_usb_device_clear(void)
{ lock_output(); clear_output(); unlock_output(); }
bool kms_usb_device_take_fault(void)
{ lock_output(); bool fault = stalled; stalled = false; unlock_output(); return fault; }
void kms_usb_device_tick(void)
{ lock_output(); tick_output(false); unlock_output(); }
static bool prepare_output(void)
{
    if (deferred_motion_activity) { deferred_motion_activity = false; kms_link_jiggler_activity(); }
    bool usb_mounted = tud_mounted(), usb_suspended = tud_suspended();
    if (usb_mounted != previous_mount)
        kms_slave_diagnostics_event(usb_mounted ? KMS_S_USB_MOUNT : KMS_S_USB_UNMOUNT, 0);
    if (usb_mounted && usb_suspended != previous_suspend)
        kms_slave_diagnostics_event(usb_suspended ? KMS_S_USB_SUSPEND : KMS_S_USB_RESUME, 0);
    previous_mount = usb_mounted; previous_suspend = usb_suspended;
    kms_slave_diagnostics_set(KMS_S_USB_FLAGS, (usb_mounted ? 1u : 0u) | (usb_suspended ? 2u : 0u));
    if (!usb_suspended) wake_requested = false;
    bool mounted = usb_mounted && !usb_suspended;
    // Preserve enabled state, but restart the idle wait while the OS is suspending.
    if (!kms_desktop_jiggler_allowed()) kms_link_jiggler_activity();
    if (!mounted) {
        if (kms_link_text_active()) stalled = true;
        text_report = false;
        head = count = waiting_interfaces = 0; active = false; memset(&sent, 0, sizeof(sent));
        portENTER_CRITICAL(&state_lock); memset(&host_state, 0, sizeof(host_state)); portEXIT_CRITICAL(&state_lock);
        was_ready = false; return false;
    }
    if (!was_ready || keyboard_protocol != tud_hid_n_get_protocol(ITF_KEYBOARD) ||
        mouse_protocol != tud_hid_n_get_protocol(ITF_MOUSE)) {
        force_neutral = true; keyboard_protocol = tud_hid_n_get_protocol(ITF_KEYBOARD);
        mouse_protocol = tud_hid_n_get_protocol(ITF_MOUSE);
    }
    was_ready = true;
    if ((active && kms_now() - pending_at >= KMS_USB_MAX_AGE_MS) ||
        (count && kms_now() - queued_at[head] >= KMS_USB_MAX_AGE_MS)) {
        clear_output(); stalled = true;
    }
    return true;
}
static void tick_output(bool completion_context)
{
    if (completion_context) {
        if (text_report || force_neutral || !tud_mounted() || tud_suspended() ||
            keyboard_protocol != tud_hid_n_get_protocol(ITF_KEYBOARD) ||
            mouse_protocol != tud_hid_n_get_protocol(ITF_MOUSE) ||
            (active && kms_time_elapsed(kms_now(), pending_at, KMS_USB_MAX_AGE_MS)) ||
            (count && kms_time_elapsed(kms_now(), queued_at[head], KMS_USB_MAX_AGE_MS))) return;
    } else if (!prepare_output()) return;
    // Finish a completed sample and begin the next in the same poll. Reports
    // still wait for endpoint completion; there is no extra empty loop per key.
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned instance = 0; instance < ITF_COUNT; ++instance)
            if ((waiting_interfaces & (1u << instance)) && !tud_hid_n_ready(instance)) return;
        waiting_interfaces = 0;
        if (force_neutral) {
            if (kms_link_text_active()) stalled = true;
            text_report = false;
            head = count = 0; memset(&pending, 0, sizeof(pending));
            memset(&sent, 0xff, sizeof(sent)); active = true; phase = 0; force_neutral = false; pending_at = kms_now();
        }
        if (!active) {
            text_report = false;
            // Completion callbacks can dequeue guarded input; application-only
            // sources require the ordinary tick.
            if (count) {
                pending = queue[head]; pending_at = queued_at[head];
                head = (head + 1) % KMS_USB_QUEUE_DEPTH; --count;
            } else if (completion_context) return;
            else if (kms_link_text_next(&pending)) {
                text_report = true; pending_at = kms_now();
            } else {
                kms_jiggler_motion_t jiggle;
                if (!kms_desktop_jiggler_allowed() || !kms_link_jiggler_motion(&jiggle)) return;
                pending = (kms_sample_t){.state = sent}; pending_at = kms_now();
            }
            active = true; phase = 0;
        }
        // Complete the keyboard before its mouse chord. Mouse and controls can
        // then use their independent endpoints together; both must complete
        // before the next sample changes the keyboard or any held state.
        uint8_t data[KEYBOARD_BYTES] = {0}; unsigned n = 0, instance = 0, id = 0;
        while (phase < 4 && !n) {
            if (phase == 0) {
                if (sent.modifiers != pending.state.modifiers || memcmp(sent.keys, pending.state.keys, KMS_KEY_BYTES)) {
                    instance = ITF_KEYBOARD; n = keyboard_report(&pending.state, data);
                }
            } else if (phase == 1) {
                bool real_motion = pending.x || pending.y || pending.wheel || pending.pan;
                kms_jiggler_motion_t jiggle = {0};
                bool jiggle_due = !completion_context && !real_motion && kms_desktop_jiggler_allowed() && kms_link_jiggler_motion(&jiggle);
                if (sent.buttons != pending.state.buttons || real_motion || jiggle_due) {
                    instance = ITF_MOUSE;
                    bool boot = tud_hid_n_get_protocol(instance) == HID_PROTOCOL_BOOT;
                    int32_t x = chunk(pending.x, boot ? INT8_MAX : INT16_MAX), y = chunk(pending.y, boot ? INT8_MAX : INT16_MAX);
                    int32_t w = chunk(pending.wheel, INT8_MAX), p = chunk(pending.pan, INT8_MAX);
                    x += jiggle.x; y += jiggle.y;
                    data[0] = pending.state.buttons;
                    if (boot) { data[1] = (uint8_t)x; data[2] = (uint8_t)y; n = BOOT_MOUSE_BYTES; }
                    else { data[1] = pending.state.buttons >> 8; data[2] = x; data[3] = (uint32_t)x >> 8;
                        data[4] = y; data[5] = (uint32_t)y >> 8; data[6] = w; data[7] = p; n = MOUSE_BYTES; }
                    if (!tud_hid_n_ready(instance)) return;
                    if (!tud_hid_n_report(instance, 0, data, n)) return;
                    kms_test_record(KMS_T_USB_SUBMIT, instance, 0, 0, data, n);
                    pending.x -= x - jiggle.x; pending.y -= y - jiggle.y;
                    if (jiggle_due) kms_link_jiggler_moved();
                    // Include time spent draining real motion through a busy USB host.
                    if (real_motion) {
                        if (completion_context) deferred_motion_activity = true;
                        else kms_link_jiggler_activity();
                    }
                    pending.wheel -= boot ? pending.wheel : w; pending.pan -= boot ? pending.pan : p;
                    sent.buttons = pending.state.buttons;
                    waiting_interfaces |= 1u << instance;
                    if (pending.x || pending.y || pending.wheel || pending.pan) return;
                    ++phase; n = 0; continue;
                }
            } else if (phase == 2) {
                if (memcmp(sent.consumer, pending.state.consumer, sizeof(sent.consumer))) {
                    instance = ITF_CONTROLS; id = REPORT_CONSUMER; n = sizeof(sent.consumer); memcpy(data, pending.state.consumer, n);
                }
            } else if (sent.system != pending.state.system) {
                instance = ITF_CONTROLS; id = REPORT_SYSTEM; n = 1; data[0] = pending.state.system;
            }
            if (!n) ++phase;
        }
        if (n) {
            // Preserve the pre-batch spacing between synthetic key presses/releases.
            if (text_report && instance == ITF_KEYBOARD && kms_now() - text_sent_at < TEXT_STEP_MS) return;
            if (!tud_hid_n_ready(instance) || !tud_hid_n_report(instance, id, data, n)) return;
            if (text_report && instance == ITF_KEYBOARD) text_sent_at = kms_now();
            kms_test_record(KMS_T_USB_SUBMIT, instance, id, 0, data, n);
            if (phase == 0) { sent.modifiers = pending.state.modifiers; memcpy(sent.keys, pending.state.keys, KMS_KEY_BYTES); }
            else if (phase == 2) memcpy(sent.consumer, pending.state.consumer, sizeof(sent.consumer));
            else sent.system = pending.state.system;
            ++phase; waiting_interfaces |= 1u << instance; return;
        } else {
            if (waiting_interfaces) return;
            kms_slave_diagnostics_peak(KMS_S_MAX_USB_MS, kms_now() - pending_at);
            portENTER_CRITICAL(&state_lock); host_state = pending.state; portEXIT_CRITICAL(&state_lock);
            active = false;
            if (text_report) { text_report = false; kms_link_text_delivered(); }
            if (completion_context && !count) return;
        }
    }
}
