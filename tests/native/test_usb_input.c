// Real USB bridge/router with deterministic USB-task and application-task steps.
#include "esp_heap_caps.h"
#include "usb_input.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, emitted, mistakes, polls, history, saves, clears, overflows, executions;
static unsigned wakeups;
static unsigned missing_keys[KMS_KEY_TRACE_REASONS];
static unsigned missing_releases;
static void missing_change(void *ctx, unsigned page, unsigned usage, bool down)
{
    (void)page; (void)usage;
    enum kms_key_trace_reason reason = *(enum kms_key_trace_reason *)ctx;
    ++missing_keys[reason]; if (!down) ++missing_releases;
}
void kms_controller_key_trace_drop(enum kms_key_trace_reason reason, unsigned source,
    unsigned slot, const kms_state_t *before, const kms_state_t *after)
{
    (void)source; (void)slot;
    kms_key_changes(before, after, missing_change, &reason);
}
void kms_app_wake(void) { ++wakeups; }
static uint32_t now = 100;
static bool recovering, block_initial, in_worker;
static unsigned task_priority, task_stack;
static int task_core;
static bool recovery_allowed;
static unsigned selected = 2, selections;
static kms_usb_input_callbacks_t callbacks;
static kms_state_t visible;
static int32_t movement;
static kms_router_t test_router;
static kms_hid_parser_t keyboard, mouse;
typedef struct { uint64_t align; uint8_t data[64 * 256]; unsigned size, capacity, head, count, caps; } queue_t;
static queue_t queues[4];
static unsigned queue_count, internal_queue_bytes, external_queue_bytes, external_failures;
static bool psram_available;

QueueHandle_t xQueueCreate(unsigned count, unsigned bytes)
{
    if (queue_count == 4 || count * bytes > sizeof(queues[0].data)) { ++mistakes; return NULL; }
    queue_t *q = &queues[queue_count++]; memset(q, 0, sizeof(*q)); q->capacity = count; q->size = bytes;
    internal_queue_bytes += count * bytes; return q;
}
QueueHandle_t xQueueCreateWithCaps(unsigned count, unsigned bytes, unsigned caps)
{
    if (caps != (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) { ++mistakes; return NULL; }
    if (!psram_available) return NULL;
    if (external_failures) { --external_failures; return NULL; }
    QueueHandle_t result = xQueueCreate(count, bytes);
    if (result) {
        ((queue_t *)result)->caps = caps;
        internal_queue_bytes -= count * bytes; external_queue_bytes += count * bytes;
    }
    return result;
}
int xQueueSend(QueueHandle_t handle, const void *data, uint32_t wait)
{
    queue_t *q = handle; if (wait) ++mistakes;
    if (q->count == q->capacity) return pdFALSE;
    memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, data, q->size); ++q->count; return pdTRUE;
}
int xQueueReceive(QueueHandle_t handle, void *data, uint32_t wait)
{
    queue_t *q = handle; if (wait) ++mistakes;
    if (!q->count) return pdFALSE;
    memcpy(data, q->data + q->head * q->size, q->size); q->head = (q->head + 1) % q->capacity; --q->count; return pdTRUE;
}
void vQueueDelete(QueueHandle_t handle) { (void)handle; }
int xTaskCreatePinnedToCore(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle, int core)
{ (void)name; (void)arg; (void)handle; if (task != service_worker) ++mistakes; task_priority = priority; task_stack = stack; task_core = core; return pdPASS; }
void vTaskDelay(unsigned ticks) { (void)ticks; ++mistakes; }
void kms_host_assert(int condition) { if (!condition) ++mistakes; }
uint32_t kms_now(void) { return now; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void kms_diagnostics_input(const kms_state_t *old, const kms_sample_t *s, bool held_input)
{ (void)old; (void)s; (void)held_input; if (in_worker) ++mistakes; }
void kms_diagnostics_held(bool value) { (void)value; if (in_worker) ++mistakes; }
void kms_diagnostics_event(enum kms_event e, unsigned slot, uint32_t detail)
{ (void)e; (void)slot; (void)detail; if (in_worker) ++mistakes; }
void kms_diagnostics_capture_event(kms_diag_record_t *out, enum kms_event event, unsigned slot, uint32_t detail)
{ if (in_worker) ++mistakes; *out = (kms_diag_record_t){.version = 1, .boot = 123, .uptime_ms = now,
    .idle_ms = 50, .event = (uint16_t)event, .slot = (uint16_t)slot, .detail = detail, .repeats = 1}; }
void kms_diagnostics_recover_event(const kms_diag_record_t *record)
{ if (in_worker || record->boot != 123) ++mistakes; ++history; }
void kms_diagnostics_save(void) { if (in_worker) ++mistakes; ++saves; }
unsigned kms_link_selected(void) { if (in_worker) ++mistakes; return selected; }
void kms_link_clear_input(void) { if (in_worker) ++mistakes; ++clears; memset(&visible, 0, sizeof(visible)); }
void kms_link_input_overflow(void) { if (in_worker) ++mistakes; ++overflows; memset(&visible, 0, sizeof(visible)); }
bool kms_link_pairing(void) { if (in_worker) ++mistakes; return false; }
bool kms_link_keyboard_leds(uint8_t *leds) { *leds = 5; if (in_worker) ++mistakes; return true; }
bool kms_remote_master_updating(void) { if (in_worker) ++mistakes; return false; }
bool kms_ota_confirmed(void) { if (in_worker) ++mistakes; return true; }
bool kms_ota_busy(void) { if (in_worker) ++mistakes; return false; }
void kms_usb_recovery_init(void) { }
void kms_usb_recovery_tick(bool allowed) { if (!in_worker) ++mistakes; recovery_allowed = allowed; }
void kms_usb_recovery_watch_begin(bool allowed) { (void)allowed; if (!in_worker) ++mistakes; }
void kms_usb_recovery_watch_end(void) { if (!in_worker) ++mistakes; }
bool kms_usb_recovery_active(void) { return recovering; }
bool kms_usb_recovery_block_initial(void) { return block_initial; }
void kms_usb_console_init(void) { if (!in_worker) ++mistakes; }
bool kms_usb_console_execute(const char *line) { (void)line; if (!in_worker) ++mistakes; ++executions; return true; }
static unsigned device_commands, device_replies;
void kms_device_usb_request(kms_desktop_message_t *m, unsigned source, uint32_t boot)
{
    if (!in_worker || source != 2 || boot != 10 || m->op != KMS_D_USB_SNAPSHOT) ++mistakes;
    m->data[0] = 42; ++device_commands;
}
void kms_desktop_device_reply(unsigned source, uint32_t boot, const kms_desktop_message_t *m)
{
    if (in_worker || source != 2 || boot != 10 || m->id != 8 || m->data[0] != 42) ++mistakes;
    ++device_replies;
}
void kms_usb_bus_sample(void) { if (!in_worker) ++mistakes; }
void kms_usb_leds_init(void) { if (!in_worker) ++mistakes; }
void kms_usb_leds_set_enabled(bool value) { if (!in_worker || value != application.enabled) ++mistakes; }
void kms_usb_leds_attach(void *o, uint8_t a, uint8_t n, const kms_hid_parser_t *p)
{ (void)o; (void)a; (void)n; (void)p; if (!in_worker) ++mistakes; }
void kms_usb_leds_detach(void *o) { (void)o; if (!in_worker) ++mistakes; }
void kms_usb_leds_tick(bool known, uint8_t leds)
{ if (!in_worker || known != application.known || leds != application.leds) ++mistakes; }
void kms_usb_devices_init(const kms_usb_input_callbacks_t *c) { if (!in_worker) ++mistakes; callbacks = *c; }
void kms_usb_devices_tick(void) { if (!in_worker) ++mistakes; ++polls; }
uint8_t kms_usb_device_address(const kms_usb_interface_t *i) { (void)i; return 1; }
uint8_t kms_usb_interface_number(const kms_usb_interface_t *i) { (void)i; return 0; }
static void emit_input(void *ctx, const kms_state_t *s, int32_t x, int32_t y, int32_t wheel, int32_t pan)
{ (void)ctx; (void)y; (void)wheel; (void)pan; if (in_worker) ++mistakes; visible = *s; movement = x; ++emitted; }
static bool select_input(void *ctx, unsigned slot)
{ (void)ctx; if (in_worker) ++mistakes; selected = slot; ++selections; return true; }
static const uint8_t key_descriptor[] = {5,1,9,6,0xa1,1,5,7,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,1,0x81,0,0xc0};
static const uint8_t mouse_descriptor[] = {5,1,9,2,0xa1,1,5,9,0x19,1,0x29,3,0x15,0,0x25,1,0x75,1,0x95,3,0x81,2,
    0x75,5,0x95,1,0x81,1,5,1,9,0x30,9,0x31,0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0};
static void attach_source(kms_usb_interface_t *source, const kms_hid_parser_t *parser)
{ in_worker = true; callbacks.attached(source, parser); in_worker = false; }
static void remove_source(kms_usb_interface_t *source)
{ in_worker = true; callbacks.removed(source); in_worker = false; }
static bool produce(kms_usb_interface_t *source, const kms_hid_parser_t *parser, const uint8_t *data, size_t size)
{ in_worker = true; bool accepted = callbacks.report(source, parser, data, size, now); in_worker = false; return accepted; }
static void pump_service(void) { in_worker = true; service_tick(); in_worker = false; }
static bool key_report(kms_usb_interface_t *source, uint8_t key)
{ bool accepted = produce(source, &keyboard, &key, 1); kms_usb_input_tick(); return accepted; }
static int queue_memory_case(bool psram, unsigned failures)
{
    // Independent boots: fixture tasks do not run until service_init below.
    queue_count = internal_queue_bytes = external_queue_bytes = 0;
    psram_available = psram; external_failures = failures;
    kms_usb_input_init(&test_router);
    CHECK(reports && commands && diagnostics && desktop_replies && queue_count == 4);
    CHECK(queues[0].capacity == KMS_USB_QUEUE_DEPTH && queues[0].size == sizeof(report_t));
    CHECK(queues[1].capacity == 4 && queues[1].size == sizeof(command_t));
    CHECK(queues[2].capacity == KMS_USB_EVENT_DEPTH && queues[2].size == sizeof(kms_diag_record_t));
    CHECK(queues[3].capacity == 4 && queues[3].size == sizeof(command_t));
    unsigned storage = KMS_USB_QUEUE_DEPTH * sizeof(report_t) + 8 * sizeof(command_t) +
        KMS_USB_EVENT_DEPTH * sizeof(kms_diag_record_t);
    CHECK(internal_queue_bytes + external_queue_bytes == storage);
    if (psram && !failures) {
        // These task-only buffers must leave internal RAM available for DMA.
        CHECK(!internal_queue_bytes && external_queue_bytes == storage);
    } else if (!psram) CHECK(internal_queue_bytes == storage && !external_queue_bytes);
    else CHECK(internal_queue_bytes == KMS_USB_QUEUE_DEPTH * sizeof(report_t) && external_queue_bytes > 0);
    return 0;
}
__declspec(dllexport) unsigned kms_usb_input_test_count(void) { return checks; }
__declspec(dllexport) int kms_run_usb_input_tests(void)
{
    static unsigned sources[3];
    kms_usb_interface_t *a = (kms_usb_interface_t *)&sources[0], *b = (kms_usb_interface_t *)&sources[1], *c = (kms_usb_interface_t *)&sources[2];
    CHECK(kms_hid_parse(&keyboard, key_descriptor, sizeof(key_descriptor)));
    CHECK(kms_hid_parse(&mouse, mouse_descriptor, sizeof(mouse_descriptor)));
    kms_router_init(&test_router, emit_input, select_input, NULL, NULL);
    int result = queue_memory_case(false, 0); if (result) return result;
    result = queue_memory_case(true, 1); if (result) return result;
    result = queue_memory_case(true, 0); if (result) return result;
    CHECK(task_priority == KMS_USB_TASK_PRIORITY && task_stack == KMS_USB_TASK_STACK && task_core == 1);
    CHECK(!polls); // Creating the owner does not service USB from the main task.
    in_worker = true; service_init(); in_worker = false;
    attach_source(a, &keyboard); uint8_t key = 4;
    unsigned before = emitted;
    unsigned wakes_before = wakeups;
    CHECK(produce(a, &keyboard, &key, 1) && emitted == before);
    CHECK(wakeups == wakes_before + 1);
    key = 0; kms_usb_input_tick(); CHECK(kms_key(&visible, 4)); // Producer storage was already changed.
    remove_source(a); CHECK(kms_key(&visible, 4)); kms_usb_input_tick(); CHECK(!kms_key(&visible, 4));
    recovering = block_initial = true; attach_source(b, &keyboard); before = emitted;
    CHECK(key_report(b, 4) && emitted == before);
    CHECK(key_report(b, 4) && emitted == before);
    recovering = false;
    CHECK(key_report(b, 4) && !kms_key(&visible, 4));
    CHECK(key_report(b, 0)); CHECK(key_report(b, 5) && kms_key(&visible, 5));
    remove_source(b); kms_usb_input_tick(); CHECK(!kms_key(&visible, 5));
    attach_source(c, &mouse); uint8_t pointer[] = {1, 7, 0}; before = emitted;
    CHECK(produce(c, &mouse, pointer, sizeof(pointer))); kms_usb_input_tick(); CHECK(emitted == before);
    pointer[1] = 3; CHECK(produce(c, &mouse, pointer, sizeof(pointer))); kms_usb_input_tick();
    CHECK(!visible.buttons && movement == 3);
    pointer[0] = 0; CHECK(produce(c, &mouse, pointer, sizeof(pointer))); kms_usb_input_tick();
    pointer[0] = 1; CHECK(produce(c, &mouse, pointer, sizeof(pointer))); kms_usb_input_tick(); CHECK(visible.buttons == 1);
    remove_source(c); kms_usb_input_tick(); CHECK(!visible.buttons);

    block_initial = false; attach_source(a, &keyboard); CHECK(key_report(a, 4) && kms_key(&visible, 4));
    // Reuse the exact source pointer before the application notices removal.
    key = 5; CHECK(produce(a, &keyboard, &key, 1));
    remove_source(a); attach_source(a, &keyboard); kms_usb_input_tick();
    CHECK(!kms_key(&visible, 4) && !kms_key(&visible, 5));
    CHECK(missing_keys[KMS_KEY_USB_INVALIDATED] >= 2);
    CHECK(key_report(a, 6) && kms_key(&visible, 6));
    before = emitted;
    for (unsigned n = 0; n < KMS_USB_QUEUE_DEPTH; ++n) CHECK(produce(a, &keyboard, &key, 1));
    wakes_before = wakeups;
    key = 0;
    unsigned releases_before = missing_releases;
    CHECK(!produce(a, &keyboard, &key, 1)); // A lost release must name the key, not an empty state.
    CHECK(missing_keys[KMS_KEY_USB_QUEUE_FULL] == 1 && missing_releases == releases_before + 1);
    CHECK(wakeups == wakes_before + 1); // Overflow also wakes the owner to release held input.
    unsigned old_polls = polls;
    for (unsigned n = 0; n < 40; ++n) pump_service();
    CHECK(polls == old_polls + 40 && emitted == before);
    kms_usb_input_tick(); CHECK(overflows == 1 && !kms_key(&visible, 6));
    CHECK(key_report(a, 5) && !kms_key(&visible, 5)); // Hold suppressed until a neutral report.
    CHECK(key_report(a, 0)); CHECK(key_report(a, 5) && kms_key(&visible, 5));

    key = 6; CHECK(produce(a, &keyboard, &key, 1)); now += KMS_USB_MAX_AGE_MS;
    kms_usb_input_tick(); CHECK(!kms_key(&visible, 6) && !kms_key(&visible, 5));
    CHECK(missing_keys[KMS_KEY_USB_STALE] >= 2);
    CHECK(key_report(a, 6) && !kms_key(&visible, 6)); CHECK(key_report(a, 0));
    CHECK(key_report(a, 7) && kms_key(&visible, 7));
    in_worker = true; kms_usb_input_block(true); kms_usb_input_event(KMS_EVENT_INPUT_ERROR, 2, 99); kms_usb_input_save(); in_worker = false;
    CHECK(!clears && !saves); kms_usb_input_tick(); CHECK(clears == 1 && saves == 1 && history >= 1 && !kms_key(&visible, 7));
    CHECK(key_report(a, 7) && !kms_key(&visible, 7)); CHECK(key_report(a, 0));
    CHECK(key_report(a, 8) && kms_key(&visible, 8));
    CHECK(kms_usb_input_command("usb-list") && !executions); pump_service(); CHECK(executions == 1 && recovery_allowed);
    kms_desktop_message_t request = {.op = KMS_D_USB_SNAPSHOT, .id = 8, .session = 3};
    CHECK(kms_usb_input_desktop(2, 10, &request) && !device_commands && !device_replies);
    pump_service(); CHECK(device_commands == 1 && !device_replies);
    kms_usb_input_desktop_replies(); CHECK(device_replies == 1);
    kms_usb_input_leds_enabled(false); pump_service(); CHECK(!application.enabled);
    remove_source(a); kms_usb_input_tick(); attach_source(a, &keyboard);
    now = 0xfffffff0u; key = 9; CHECK(produce(a, &keyboard, &key, 1));
    now += KMS_USB_MAX_AGE_MS - 1; kms_usb_input_tick(); CHECK(kms_key(&visible, 9));
    CHECK(key_report(a, 0)); // Queue age arithmetic also works across the millisecond wrap.
    kms_router_set_trigger(&test_router, KMS_TRIGGER_INSERT);
    static const uint8_t shortcut[] = {0x49, 0, 0x49, 0, KMS_HID_KEY_1, 0};
    for (unsigned n = 0; n < sizeof(shortcut); ++n) {
        now += 2; CHECK(produce(a, &keyboard, &shortcut[n], 1));
    }
    CHECK(!selections); kms_usb_input_tick(); CHECK(selections == 1 && selected == 1);
    CHECK(!kms_key(&visible, KMS_HID_KEY_1) && !kms_key(&visible, 0x49));
    unsigned previous_history = history;
    in_worker = true;
    for (unsigned n = 0; n < KMS_USB_EVENT_DEPTH + 2; ++n) kms_usb_input_event(KMS_EVENT_INPUT_ERROR, 1, n);
    in_worker = false; pump_service(); kms_usb_input_tick();
    CHECK(history == previous_history + KMS_USB_EVENT_DEPTH); // Full diagnostic queue is bounded and nonblocking too.
    CHECK(key_report(a, 4) && kms_key(&visible, 4));
    key = 5; in_worker = true;
    CHECK(callbacks.report(a, &keyboard, &key, 1, now - KMS_USB_MAX_AGE_MS));
    in_worker = false; kms_usb_input_tick();
    CHECK(!kms_key(&visible, 4) && !kms_key(&visible, 5)); // Age includes time spent in the raw-report ring.
    CHECK(key_report(a, 0)); CHECK(key_report(a, 6) && kms_key(&visible, 6));
    unsigned before_overflows = overflows;
    key = 7; CHECK(produce(a, &keyboard, &key, 1));
    in_worker = true; callbacks.overflow(); in_worker = false;
    kms_usb_input_tick();
    CHECK(overflows == before_overflows + 1 && !kms_key(&visible, 6) && !kms_key(&visible, 7));
    CHECK(key_report(a, 7) && !kms_key(&visible, 7));
    CHECK(key_report(a, 0)); CHECK(key_report(a, 7) && kms_key(&visible, 7));
    CHECK(mistakes == 0);
    return 0;
}
