#include "usb_input.h"
#include "device_controls.h"
#include "usb_devices.h"
#include "usb_console.h"
#include "usb_leds.h"
#include "usb_health.h"
#include "usb_bus.h"
#include "usb_recovery.h"
#include "kms_remote.h"
#include "kms_ota.h"
#include "kms_runtime.h"
#include "kms_diagnostic_store.h"
#include "kms_test.h"
#include "key_trace.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    kms_usb_interface_t *source;
    uint32_t generation;
    bool block_initial;
} binding_t;
typedef struct {
    uint32_t generation;
    uint8_t inputs[256];
    bool block_initial;
} route_t;
typedef struct {
    kms_sample_t sample;
    kms_state_t previous;
    uint32_t generation, revision, at;
    uint8_t binding;
    bool recovering;
} report_t;
enum { USB_COMMAND_BYTES = 80 };
typedef struct {
    unsigned source;
    uint32_t boot;
    union { char line[USB_COMMAND_BYTES]; kms_desktop_message_t desktop; };
} command_t;
static QueueHandle_t desktop_replies;
typedef struct {
    kms_diag_record_t context;
    unsigned sources;
    uint8_t leds;
    bool known, enabled, allowed;
} application_t;

static binding_t bindings[KMS_USB_HID_INTERFACES];
static route_t routes[KMS_USB_HID_INTERFACES];
static bool used[KMS_INPUTS], need_neutral[KMS_INPUTS];
static kms_router_t *router;
static QueueHandle_t reports, commands, diagnostics;
static portMUX_TYPE bridge_lock = portMUX_INITIALIZER_UNLOCKED;
static application_t application;
static uint32_t revision, consumed_revision;
static bool overflow, clear_link, save_history;
static unsigned lost_diagnostics;
// Owned by the USB worker. Preserve release transitions even when the report
// queue is full or the application later invalidates its backlog.
static struct {
    kms_state_t state;
    uint32_t generation;
    unsigned binding, report_id;
    bool used;
} trace_inputs[KMS_INPUTS];
static const char *TAG = "usb-input";

unsigned kms_usb_input_selected(void)
{
    portENTER_CRITICAL(&bridge_lock); unsigned slot = application.context.slot; portEXIT_CRITICAL(&bridge_lock);
    return slot;
}
unsigned kms_usb_input_sources(void)
{
    portENTER_CRITICAL(&bridge_lock); unsigned count = application.sources; portEXIT_CRITICAL(&bridge_lock);
    return count;
}
void kms_usb_input_capture_event(kms_diag_record_t *out, enum kms_event event, unsigned slot, uint32_t detail)
{
    portENTER_CRITICAL(&bridge_lock); *out = application.context; portEXIT_CRITICAL(&bridge_lock);
    uint32_t now = kms_now();
    if (out->idle_ms) out->idle_ms += now - out->uptime_ms;
    out->uptime_ms = now; out->event = (uint16_t)event; out->slot = (uint16_t)slot; out->detail = detail;
}
void kms_usb_input_event(enum kms_event event, unsigned slot, uint32_t detail)
{
    kms_diag_record_t record;
    kms_usb_input_capture_event(&record, event, slot, detail);
    if (xQueueSend(diagnostics, &record, 0) != pdTRUE) {
        portENTER_CRITICAL(&bridge_lock); ++lost_diagnostics; portEXIT_CRITICAL(&bridge_lock);
    }
}
void kms_usb_input_save(void)
{
    portENTER_CRITICAL(&bridge_lock); save_history = true; portEXIT_CRITICAL(&bridge_lock);
}
void kms_usb_input_block(bool clear)
{
    portENTER_CRITICAL(&bridge_lock); ++revision; clear_link |= clear; portEXIT_CRITICAL(&bridge_lock);
}
static void attached(kms_usb_interface_t *source, const kms_hid_parser_t *parser)
{
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) if (!bindings[n].source) {
        portENTER_CRITICAL(&bridge_lock);
        if (!++bindings[n].generation) ++bindings[n].generation;
        bindings[n].source = source;
        bindings[n].block_initial = kms_usb_recovery_block_initial();
        portEXIT_CRITICAL(&bridge_lock);
        kms_usb_leds_attach(source, kms_usb_device_address(source), kms_usb_interface_number(source), parser);
        return;
    }
    ESP_LOGW(TAG, "Router interface capacity reached");
}
static void removed(kms_usb_interface_t *source)
{
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) if (bindings[n].source == source) {
        kms_usb_leds_detach(source);
        portENTER_CRITICAL(&bridge_lock); bindings[n].source = NULL; portEXIT_CRITICAL(&bridge_lock);
        kms_usb_input_event(KMS_EVENT_INPUT_REMOVED, kms_usb_input_selected(), 0);
        return;
    }
}
static void report_overflow(void)
{
    portENTER_CRITICAL(&bridge_lock);
    if (!overflow) ++revision;
    overflow = true;
    portEXIT_CRITICAL(&bridge_lock);
}
static bool report(kms_usb_interface_t *source, const kms_hid_parser_t *parser, const uint8_t *data, size_t bytes, uint32_t captured_at)
{
    kms_test_record(KMS_T_RAW, kms_usb_device_address(source), kms_usb_interface_number(source), 0, data, bytes);
    report_t message = {.at = captured_at, .recovering = kms_usb_recovery_active()};
    if (!kms_hid_decode(parser, data, bytes, &message.sample)) return false;
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) if (bindings[n].source == source) {
        message.binding = (uint8_t)n; message.generation = bindings[n].generation;
        unsigned free_index = KMS_INPUTS, index = KMS_INPUTS;
        for (unsigned t = 0; t < KMS_INPUTS; ++t) {
            if (trace_inputs[t].used && trace_inputs[t].binding == n &&
                trace_inputs[t].generation == message.generation && trace_inputs[t].report_id == message.sample.report_id) {
                index = t; break;
            }
            if (!trace_inputs[t].used || !bindings[trace_inputs[t].binding].source ||
                bindings[trace_inputs[t].binding].generation != trace_inputs[t].generation) free_index = t;
        }
        if (index == KMS_INPUTS) {
            index = free_index;
            if (index < KMS_INPUTS) {
                memset(&trace_inputs[index], 0, sizeof(trace_inputs[index]));
                trace_inputs[index].used = true; trace_inputs[index].binding = n;
                trace_inputs[index].generation = message.generation;
                trace_inputs[index].report_id = message.sample.report_id;
            }
        }
        if (index < KMS_INPUTS) {
            message.previous = trace_inputs[index].state;
            trace_inputs[index].state = message.sample.state;
        } else ESP_LOGW(TAG, "Key audit incomplete: source capacity reached; previous report state unavailable");
        portENTER_CRITICAL(&bridge_lock); message.revision = revision; portEXIT_CRITICAL(&bridge_lock);
        if (xQueueSend(reports, &message, 0) == pdTRUE) { kms_app_wake(); return true; }
        // Never wait for the application or stop USB polling. Invalidate the
        // backlog so a missing release cannot leave a key/button pressed.
        report_overflow();
        kms_controller_key_trace_drop(KMS_KEY_USB_QUEUE_FULL, n + 1u, kms_usb_input_selected(),
            &message.previous, &message.sample.state);
        kms_app_wake();
        return false;
    }
    return false;
}
static void service_init(void)
{
    kms_usb_input_callbacks_t callbacks = {.attached = attached, .report = report, .removed = removed, .overflow = report_overflow};
    kms_usb_devices_init(&callbacks); kms_usb_leds_init(); kms_usb_console_init();
}
static void service_tick(void)
{
    application_t current;
    portENTER_CRITICAL(&bridge_lock); current = application; portEXIT_CRITICAL(&bridge_lock);
    kms_usb_recovery_watch_begin(current.allowed);
    kms_usb_bus_sample(); kms_usb_devices_tick();
    kms_usb_leds_set_enabled(current.enabled);
    kms_usb_leds_tick(current.known, current.leds);
    kms_usb_recovery_tick(current.allowed);
    kms_usb_recovery_watch_end();
    // USB clients and all commands which touch their handles have one owner.
    command_t command;
    if (xQueueReceive(commands, &command, 0) == pdTRUE) {
        kms_usb_recovery_watch_begin(current.allowed);
        if (command.source) {
            kms_device_usb_request(&command.desktop, command.source, command.boot);
            (void)xQueueSend(desktop_replies, &command, 0);
        } else (void)kms_usb_console_execute(command.line);
        kms_usb_recovery_watch_end();
    }
}
static void service_worker(void *arg)
{
    (void)arg; service_init();
    while (true) { service_tick(); vTaskDelay(pdMS_TO_TICKS(1)); }
}
static QueueHandle_t create_queue(unsigned count, unsigned bytes)
{
    // Only the USB owner and application tasks access these copied messages.
    // Keep their storage out of the internal heap needed by aligned USB DMA
    // lists. Queues live for the entire boot; retain the no-PSRAM fallback.
    QueueHandle_t queue = xQueueCreateWithCaps(count, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return queue ? queue : xQueueCreate(count, bytes);
}
void kms_usb_input_init(kms_router_t *r)
{
    router = r;
    reports = create_queue(KMS_USB_QUEUE_DEPTH, sizeof(report_t));
    commands = create_queue(4, sizeof(command_t));
    diagnostics = create_queue(KMS_USB_EVENT_DEPTH, sizeof(kms_diag_record_t));
    desktop_replies = create_queue(4, sizeof(command_t));
    configASSERT(reports && commands && diagnostics);
    kms_usb_health_init(); kms_usb_recovery_init();
    application.enabled = KMS_KEYBOARD_LEDS_ENABLED;
    kms_diagnostics_capture_event(&application.context, KMS_EVENT_INPUT_ERROR, kms_link_selected(), 0);
    // Allocate the USB interrupt on CPU1 as well, away from the Wi-Fi task.
    configASSERT(xTaskCreatePinnedToCore(service_worker, "usb-input", KMS_USB_TASK_STACK, NULL,
                                        KMS_USB_TASK_PRIORITY, NULL, 1) == pdPASS);
}
void kms_usb_input_leds_enabled(bool enabled)
{
    portENTER_CRITICAL(&bridge_lock); application.enabled = enabled; portEXIT_CRITICAL(&bridge_lock);
}
bool kms_usb_input_desktop(unsigned source, uint32_t boot, const kms_desktop_message_t *m)
{
    command_t command = {.source = source, .boot = boot, .desktop = *m};
    return desktop_replies && commands && xQueueSend(commands, &command, 0) == pdTRUE;
}
void kms_usb_input_desktop_replies(void)
{
    command_t command;
    while (desktop_replies && xQueueReceive(desktop_replies, &command, 0) == pdTRUE)
        kms_desktop_device_reply(command.source, command.boot, &command.desktop);
}
bool kms_usb_input_command(const char *line)
{
    size_t length = 0;
    while (length < USB_COMMAND_BYTES && line[length]) ++length;
    if (!commands || length == USB_COMMAND_BYTES) return false;
    command_t command = {0}; memcpy(command.line, line, length + 1);
    return xQueueSend(commands, &command, 0) == pdTRUE;
}
static void held(void)
{
    kms_state_t zero = {0}; bool any = false;
    for (unsigned n = 0; n < KMS_INPUTS; ++n)
        if (!kms_state_equal(&router->inputs[n].physical, &zero)) any = true;
    kms_diagnostics_held(any);
}
static void reconcile(const binding_t *current)
{
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) {
        route_t *route = &routes[n];
        if (route->generation == current[n].generation && current[n].source) continue;
        for (unsigned id = 0; id < 256; ++id) if (route->inputs[id]) {
            unsigned index = route->inputs[id] - 1;
            kms_router_remove(router, index); used[index] = need_neutral[index] = false;
        }
        memset(route, 0, sizeof(*route));
        if (current[n].source) {
            route->generation = current[n].generation;
            route->block_initial = current[n].block_initial;
        }
    }
    held();
}
static void route_report(const report_t *message)
{
    route_t *route = &routes[message->binding];
    if (!route->generation || route->generation != message->generation) return;
    const kms_sample_t *sample = &message->sample;
    unsigned id = sample->report_id;
    bool first = !route->inputs[id];
    if (first) {
        for (unsigned n = 0; n < KMS_INPUTS; ++n) if (!used[n]) { used[n] = true; route->inputs[id] = n + 1; break; }
        if (!route->inputs[id]) {
            kms_controller_key_trace_drop(KMS_KEY_SOURCE_CAPACITY, message->binding + 1u,
                kms_link_selected(), &message->previous, &sample->state);
            ESP_LOGW(TAG, "Report source capacity reached"); return;
        }
    }
    unsigned index = route->inputs[id] - 1;
    kms_test_sample(KMS_T_DECODED, index, sample);
    if (kms_time_elapsed(kms_now(), message->at, KMS_USB_MAX_AGE_MS)) {
        kms_controller_key_trace_drop(KMS_KEY_USB_STALE, index + 1u, kms_link_selected(),
            &message->previous, &sample->state);
        kms_usb_input_block(false);
        kms_router_block_held(router);
        for (unsigned n = 0; n < KMS_INPUTS; ++n) need_neutral[n] = used[n];
        for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) routes[n].block_initial = true;
        kms_link_input_overflow(); return;
    }
    kms_state_t zero = {0}; bool any = !kms_state_equal(&sample->state, &zero);
    for (unsigned n = 0; n < KMS_INPUTS; ++n)
        if (n != index && !kms_state_equal(&router->inputs[n].physical, &zero)) any = true;
    kms_diagnostics_input(&router->inputs[index].physical, sample, any);
    if (message->recovering || (first && route->block_initial)) {
        kms_controller_key_trace_drop(KMS_KEY_USB_RECOVERY, index + 1u, kms_link_selected(),
            &message->previous, &sample->state);
        router->inputs[index].physical = router->inputs[index].blocked = sample->state;
        memset(&router->inputs[index].visible, 0, sizeof(kms_state_t)); return;
    }
    if (need_neutral[index]) {
        if (!kms_state_equal(&sample->state, &zero)) {
            kms_controller_key_trace_drop(KMS_KEY_HELD_BLOCKED, index + 1u, kms_link_selected(),
                &message->previous, &sample->state);
            return;
        }
        need_neutral[index] = false;
    }
    kms_router_input(router, index, sample, kms_now());
}
void kms_usb_input_tick(void)
{
    kms_diag_record_t context;
    kms_diagnostics_capture_event(&context, KMS_EVENT_INPUT_ERROR, kms_link_selected(), 0);
    uint8_t leds; bool known = kms_link_keyboard_leds(&leds);
    bool allowed = !kms_remote_master_updating() && kms_ota_confirmed() && !kms_ota_busy() && !kms_link_pairing();
    unsigned sources = 0;
    for (unsigned n = 0; n < KMS_INPUTS; ++n) sources += used[n];
    binding_t current[KMS_USB_HID_INTERFACES];
    portENTER_CRITICAL(&bridge_lock);
    application.context = context; application.known = known; application.leds = leds; application.allowed = allowed;
    application.sources = sources;
    memcpy(current, bindings, sizeof(current));
    uint32_t next_revision = revision;
    bool lost = overflow, clear = clear_link, save = save_history;
    unsigned lost_events = lost_diagnostics;
    overflow = clear_link = save_history = false; lost_diagnostics = 0;
    portEXIT_CRITICAL(&bridge_lock);
    reconcile(current);
    if (next_revision != consumed_revision) {
        consumed_revision = next_revision; kms_router_block_held(router);
        for (unsigned n = 0; n < KMS_INPUTS; ++n) need_neutral[n] = used[n];
        for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) routes[n].block_initial = true;
    }
    if (clear) kms_link_clear_input();
    if (lost) kms_link_input_overflow();
    kms_diag_record_t record;
    for (unsigned n = 0; n < KMS_USB_EVENT_DEPTH && xQueueReceive(diagnostics, &record, 0) == pdTRUE; ++n)
        kms_diagnostics_recover_event(&record);
    if (lost_events) ESP_LOGW(TAG, "%u USB history records dropped; fault snapshot remains available", lost_events);
    if (save) kms_diagnostics_save();
    report_t message;
    for (unsigned n = 0; n < KMS_USB_QUEUE_DEPTH && xQueueReceive(reports, &message, 0) == pdTRUE; ++n) {
        portENTER_CRITICAL(&bridge_lock);
        bool valid = message.revision == revision && message.revision == consumed_revision &&
                     bindings[message.binding].source && bindings[message.binding].generation == message.generation;
        portEXIT_CRITICAL(&bridge_lock);
        if (valid) route_report(&message);
        else kms_controller_key_trace_drop(KMS_KEY_USB_INVALIDATED, message.binding + 1u,
            kms_link_selected(), &message.previous, &message.sample.state);
    }
}
void kms_usb_input_status(void)
{
    if (!kms_usb_input_command("usb-status")) ESP_LOGW(TAG, "USB status busy; retry shortly");
}
