#include "usb_devices.h"
#include "usb_preferences.h"
#include "usb_health.h"
#include "usb_memory.h"
#include "kms_runtime.h"
#include "kms_diagnostic_store.h"
#include "usb_input.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

enum { HID_CLASS = 3, HUB_CLASS = 9, HID_DESCRIPTOR = 0x21, REPORT_DESCRIPTOR = 0x22,
       SET_PROTOCOL = 11, CONTROL_REPORT = 1, CONTROL_PROTOCOL = 2, CONTROL_HEALTH = 3 };
typedef struct device device_t;
struct kms_usb_interface {
    device_t *device;
    kms_usb_interface_info_t info;
    kms_hid_parser_t *parser;
    usb_transfer_t *transfer;
    uint16_t packet_bytes;
    uint8_t endpoint, subclass;
    uint32_t generation;
    esp_err_t submit_result;
    bool selected, probed, claimed, active, stopping, halted, inflight, completed, release_warned;
};
struct device {
    usb_device_handle_t handle;
    kms_usb_device_info_t info;
    usb_transfer_t *control;
    kms_usb_interface_t *control_interface;
    uint32_t submitted, last_probe, retry_since, allocation_sequence;
    uint8_t control_phase, probe_failures;
    bool gone, control_busy, control_done, control_warned, probe_ok, retrying, allocation_warned;
};
static device_t devices[KMS_USB_DEVICES];
static kms_usb_device_info_t hubs[KMS_USB_DEVICES];
static unsigned hub_count;
static kms_usb_interface_t interfaces[KMS_USB_INTERFACES];
static bool pending[128], plan_dirty;
static unsigned planned_baseline;
static bool quiescing;
static usb_host_client_handle_t client;
static kms_usb_policy_t policy;
static kms_usb_input_callbacks_t input;
typedef struct {
    kms_usb_interface_t *source;
    uint32_t generation, captured_at;
    size_t bytes;
    uint8_t data[KMS_REPORT_BYTES];
} raw_report_t;
// Both completion callbacks and this ring's consumer run on the USB owner.
// No heap allocation, locks or waits are needed to transfer report ownership.
static raw_report_t raw_reports[KMS_USB_RAW_QUEUE_DEPTH];
static unsigned raw_head, raw_count, next_delay;
static bool raw_overflow;
static uint32_t next_generation;
static int64_t last_dispatch_end;
static kms_usb_stream_status_t stream;
static const char *TAG = "usb-input";
static void error(kms_usb_interface_t *, const char *, esp_err_t);
static bool submit_report(kms_usb_interface_t *);
static uint32_t timing(kms_usb_peak_t *out, int64_t started)
{
    int64_t elapsed = esp_timer_get_time() - started;
    uint32_t us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    if (us > out->max_us) { out->max_us = us; out->at = kms_now(); }
    return us;
}

const char *kms_usb_interface_state(unsigned state)
{
    static const char *const names[] = {"probing", "active", "off", "channel-limit",
        "unsupported", "error", "removing", "ignored", "standby"};
    return state < sizeof(names) / sizeof(*names) ? names[state] : "error";
}
uint8_t kms_usb_device_address(const kms_usb_interface_t *i) { return i->info.address; }
uint8_t kms_usb_interface_number(const kms_usb_interface_t *i) { return i->info.number; }
static kms_usb_rule_t key(const kms_usb_interface_t *i)
{
    return (kms_usb_rule_t){.serial = i->device->info.serial, .vid = i->device->info.vid,
        .pid = i->device->info.pid, .interface_number = i->info.number};
}
static void client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV && event->new_dev.address < 128)
        pending[event->new_dev.address] = true;
    else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
        for (unsigned n = 0; n < KMS_USB_DEVICES; ++n)
            if (devices[n].handle == event->dev_gone.dev_hdl) { devices[n].gone = true; plan_dirty = true; }
}
static void daemon(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &flags));
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) (void)usb_host_device_free_all();
    }
}
static void control_done(usb_transfer_t *transfer)
{
    device_t *d = transfer->context;
    kms_usb_health_complete(d, transfer->status, transfer->actual_num_bytes, transfer->num_bytes);
    d->control_busy = false; d->control_done = true;
}
static void report_done(usb_transfer_t *transfer)
{
    int64_t started = esp_timer_get_time();
    kms_usb_interface_t *i = transfer->context;
    i->info.last_complete = kms_now(); i->info.completed_once = true;
    i->info.last_status = transfer->status; i->info.last_bytes = transfer->actual_num_bytes;
    i->inflight = false; i->completed = true; i->submit_result = ESP_OK;
    if (!i->active || i->stopping || i->device->gone || quiescing ||
        transfer->status != USB_TRANSFER_STATUS_COMPLETED) { timing(&stream.callback, started); return; }
    int bytes = transfer->actual_num_bytes;
    bool valid = bytes > 0 && bytes <= i->packet_bytes && bytes <= KMS_REPORT_BYTES;
    raw_report_t *message = valid && !raw_overflow && raw_count < KMS_USB_RAW_QUEUE_DEPTH ?
        &raw_reports[(raw_head + raw_count) % KMS_USB_RAW_QUEUE_DEPTH] : NULL;
    if (message) {
        message->source = i; message->generation = i->generation;
        message->captured_at = i->info.last_complete; message->bytes = (size_t)bytes;
        memcpy(message->data, transfer->data_buffer, (size_t)bytes);
    }
    ++i->info.reports; ++stream.completed; stream.last_complete = i->info.last_complete;
    // Only copy, resubmit and publish here. In particular, no HID decoding,
    // test tracing, application callback, logging or endpoint teardown runs
    // inside the SDK callback, even on queue overflow or submission failure.
    i->submit_result = usb_host_transfer_submit(transfer);
    if (i->submit_result == ESP_OK) {
        i->inflight = true; i->completed = false; i->info.last_submit = kms_now();
        if (message) {
            ++raw_count;
            if (raw_count > stream.peak) stream.peak = raw_count;
        } else if (valid) {
            if (!raw_overflow) ++stream.overflows;
            raw_overflow = true; ++stream.dropped;
        }
    }
    timing(&stream.callback, started);
}
static void process_reports(void)
{
    if (raw_overflow) {
        stream.dropped += raw_count; raw_count = raw_head = 0; raw_overflow = false;
        if (input.overflow) input.overflow();
        return;
    }
    for (unsigned n = 0; n < KMS_USB_RAW_QUEUE_DEPTH && raw_count; ++n) {
        const raw_report_t *message = &raw_reports[raw_head];
        kms_usb_interface_t *i = message->source;
        if (!i->device || i->device->gone || !i->active || i->generation != message->generation) ++stream.stale;
        else if (kms_time_elapsed(kms_now(), message->captured_at, KMS_USB_MAX_AGE_MS)) {
            ++stream.expired; stream.dropped += raw_count;
            raw_count = raw_head = 0;
            if (input.overflow) input.overflow();
            return;
        } else {
            int64_t started = esp_timer_get_time();
            if (input.report(i, i->parser, message->data, message->bytes, message->captured_at)) ++i->info.decoded;
            timing(&stream.decode, started);
        }
        raw_head = (raw_head + 1) % KMS_USB_RAW_QUEUE_DEPTH; --raw_count;
    }
}
static void stop(kms_usb_interface_t *i, unsigned state)
{
    i->generation = 0;
    if (i->active) { i->active = false; input.removed(i); }
    i->info.state = state;
    if (!i->claimed) return;
    i->stopping = true;
    if (!i->halted && !quiescing) {
        // Recovery disconnects the root port first. A halt on the stalled bus
        // can wait forever inside the SDK; let disconnect cancel these URBs.
        // A physically removed endpoint can already be invalid. Still drain its
        // completion before releasing any transfer buffer or device reference.
        (void)usb_host_endpoint_halt(i->device->handle, i->endpoint);
        (void)usb_host_endpoint_flush(i->device->handle, i->endpoint);
        i->halted = true;
    }
}
static void error(kms_usb_interface_t *i, const char *operation, esp_err_t result)
{
    device_t *d = i->device;
    if (result == ESP_ERR_NO_MEM && !d->gone) {
        kms_usb_memory_failed(i->info.address, i->info.number, d->allocation_sequence);
        // Heap/DMA allocation can fail while old endpoints are draining or
        // after fragmentation from hot-plug. Do not latch the whole physical
        // receiver into ERROR. Retire its bundle and retry after releasing it.
        if (!d->allocation_warned) {
            ESP_LOGW(TAG, "%u/%u %s waiting for memory; retrying after cleanup", i->info.address,
                     i->info.number, operation);
            kms_usb_input_event(KMS_EVENT_INPUT_ERROR, kms_usb_input_selected(),
                               (uint32_t)i->info.address << 16 | (uint32_t)i->info.number << 8 | (result & 255));
            d->allocation_warned = true;
        }
        stop(i, KMS_USB_PROBING); i->probed = false;
        d->retrying = true; d->retry_since = kms_now(); plan_dirty = true;
        return;
    }
    ESP_LOGW(TAG, "%u/%u %s failed: %s", i->info.address, i->info.number,
             operation, esp_err_to_name(result));
    kms_usb_input_event(KMS_EVENT_INPUT_ERROR, kms_usb_input_selected(),
                          (uint32_t)i->info.address << 16 | (uint32_t)i->info.number << 8 | (result & 255));
    stop(i, KMS_USB_ERROR); i->probed = true; plan_dirty = true;
}
static bool submit_report(kms_usb_interface_t *i)
{
    esp_err_t result = usb_host_transfer_submit(i->transfer);
    if (result != ESP_OK) { error(i, "input submission", result); return false; }
    i->submit_result = ESP_OK;
    i->inflight = true; i->info.last_submit = kms_now(); return true;
}
static bool finish_stop(kms_usb_interface_t *i)
{
    device_t *d = i->device;
    if (i->inflight || (d->control_interface == i && (d->control_busy || d->control_done))) return false;
    if (i->claimed) {
        esp_err_t result = usb_host_interface_release(client, d->handle, i->info.number);
        if (result != ESP_OK) {
            if (!i->release_warned) {
                ESP_LOGW(TAG, "%u/%u waiting to release endpoint: %s", i->info.address,
                         i->info.number, esp_err_to_name(result));
                i->release_warned = true;
            }
            return false;
        }
        i->claimed = false;
    }
    if (i->transfer) {
        if (usb_host_transfer_free(i->transfer) != ESP_OK) return false;
        i->transfer = NULL;
    }
    free(i->parser); i->parser = NULL;
    i->stopping = i->halted = i->completed = i->release_warned = false;
    return true;
}
static void activate(kms_usb_interface_t *i)
{
    if (!i->selected || !i->info.wanted || i->device->gone) { stop(i, KMS_USB_OFF); return; }
    i->active = true; i->info.state = KMS_USB_ACTIVE;
    if (!++next_generation) ++next_generation;
    i->generation = next_generation;
    bool complete = true;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].device == i->device && interfaces[n].selected && !interfaces[n].active) complete = false;
    if (complete) i->device->allocation_warned = false;
    input.attached(i, i->parser);
    ESP_LOGI(TAG, "Address %u interface %u active: %u fields, %u endpoint channels",
             i->info.address, i->info.number, i->parser->count, i->info.endpoints);
}
static bool control(kms_usb_interface_t *i, unsigned phase)
{
    device_t *d = i->device;
    size_t bytes = phase == CONTROL_REPORT ? i->info.descriptor_bytes : phase == CONTROL_HEALTH ? 2 : 0;
    if (!d->control || d->control->data_buffer_size < USB_SETUP_PACKET_SIZE + bytes) {
        if (d->control && usb_host_transfer_free(d->control) != ESP_OK) return false;
        d->control = NULL;
        d->allocation_sequence = kms_usb_memory_sequence();
        if (usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + bytes, 0, &d->control) != ESP_OK) {
            error(i, "descriptor memory", ESP_ERR_NO_MEM); return false;
        }
    }
    usb_transfer_t *t = d->control;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    *s = (usb_setup_packet_t){.bmRequestType = phase == CONTROL_REPORT ? 0x81 : 0x21,
        .bRequest = phase == CONTROL_REPORT ? USB_B_REQUEST_GET_DESCRIPTOR : SET_PROTOCOL,
        .wValue = phase == CONTROL_REPORT ? REPORT_DESCRIPTOR << 8 : 1,
        .wIndex = i->info.number, .wLength = bytes};
    if (phase == CONTROL_HEALTH) *s = (usb_setup_packet_t){.bmRequestType = 0x80, .bRequest = 0, .wLength = 2};
    t->device_handle = d->handle; t->bEndpointAddress = 0;
    t->num_bytes = USB_SETUP_PACKET_SIZE + bytes; t->context = d; t->callback = control_done;
    d->control_interface = i; d->control_phase = phase;
    esp_err_t result = usb_host_transfer_submit_control(client, t);
    if (result != ESP_OK) {
        kms_usb_health_rejected(phase == CONTROL_REPORT ? KMS_USB_DESCRIPTOR : phase == CONTROL_PROTOCOL ? KMS_USB_PROTOCOL : KMS_USB_PROBE,
                               d->info.address, i->info.number, s, result);
        d->control_interface = NULL; error(i, "control submission", result); return false;
    }
    d->control_busy = true; d->control_warned = false; d->submitted = kms_now();
    kms_usb_health_begin(d, phase == CONTROL_REPORT ? KMS_USB_DESCRIPTOR : phase == CONTROL_PROTOCOL ? KMS_USB_PROTOCOL : KMS_USB_PROBE,
                         d->info.address, i->info.number);
    kms_usb_health_request(d, s, t->num_bytes);
    return true;
}
static unsigned channels_used(void)
{
    usb_host_lib_info_t host;
    unsigned count = usb_host_lib_info(&host) == ESP_OK ? (unsigned)host.num_devices : KMS_USB_CHANNELS;
    for (unsigned n = 0; n < hub_count; ++n) count += hubs[n].hub_endpoints;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].claimed) count += interfaces[n].info.endpoints;
    return count;
}
static unsigned claimed_devices(void)
{
    unsigned count = 0;
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n) {
        bool claimed = false;
        for (unsigned j = 0; j < KMS_USB_INTERFACES; ++j)
            if (interfaces[j].device == &devices[n] && interfaces[j].claimed) claimed = true;
        if (claimed) ++count;
    }
    return count;
}
static bool can_claim_device(const kms_usb_interface_t *i)
{
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].device == i->device && interfaces[n].claimed) return true;
    return claimed_devices() < KMS_USB_ACTIVE_DEVICES;
}
static bool device_ready(const device_t *d)
{
    if (d->retrying) return false;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        const kms_usb_interface_t *i = &interfaces[n];
        if (i->device == d && i->info.device_class == HID_CLASS &&
            ((!i->probed && !i->info.kinds) || i->info.state == KMS_USB_ERROR)) return false;
    }
    return true;
}
static void reconcile_selection(void)
{
    unsigned baseline = channels_used();
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].claimed) baseline -= interfaces[n].info.endpoints;
    // Device destruction runs on the USB daemon, after our disconnect callback.
    // Replan when those control/hub channels actually become available.
    if (!plan_dirty && baseline == planned_baseline) return;
    plan_dirty = false; planned_baseline = baseline;
    static kms_usb_candidate_t candidates[KMS_USB_INTERFACES];
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        kms_usb_interface_t *i = &interfaces[n];
        bool valid = i->device && !i->device->gone && device_ready(i->device) && i->info.device_class == HID_CLASS &&
            i->info.state != KMS_USB_ERROR && i->info.state != KMS_USB_UNSUPPORTED;
        kms_usb_rule_t identity = valid ? key(i) : (kms_usb_rule_t){0};
        candidates[n] = (kms_usb_candidate_t){.device = i->info.address, .endpoints = i->info.endpoints,
            .kinds = i->info.kinds, .enabled = valid && i->info.wanted,
            .preferred = valid && kms_usb_policy_override(&policy, &identity) == 1, .active = i->active};
    }
    uint32_t selected = kms_usb_plan(candidates, KMS_USB_INTERFACES, baseline);
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        kms_usb_interface_t *i = &interfaces[n];
        bool use = (selected & (1u << n)) != 0;
        if (i->selected == use) continue;
        i->selected = use;
        if (use) { i->probed = false; i->info.state = KMS_USB_PROBING; }
        else {
            unsigned state = i->info.state == KMS_USB_ERROR || i->info.state == KMS_USB_UNSUPPORTED ?
                i->info.state : i->info.wanted ? KMS_USB_STANDBY : KMS_USB_OFF;
            stop(i, state);
        }
    }
}
static bool cleanup_pending(void)
{
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n)
        if (devices[n].handle && devices[n].gone) return true;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].stopping) return true;
    return false;
}
static bool retry_ready(const device_t *d)
{
    if (!kms_time_elapsed(kms_now(), d->retry_since, KMS_USB_ALLOC_RETRY_MS) ||
        d->control_busy || d->control_done) return false;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].device == d && (interfaces[n].claimed || interfaces[n].stopping)) return false;
    return true;
}
static bool claim(kms_usb_interface_t *i)
{
    // A disconnect notification can arrive while old callbacks still own
    // endpoints and buffers. Drain that ownership before asking the SDK to
    // allocate replacement pipes and aligned DMA lists.
    if (cleanup_pending() || !can_claim_device(i)) {
        i->info.state = KMS_USB_STANDBY; return false;
    }
    if (channels_used() + i->info.endpoints > KMS_USB_CHANNELS) {
        i->info.state = KMS_USB_LIMIT;
        return false;
    }
    i->device->allocation_sequence = kms_usb_memory_sequence();
    esp_err_t result = usb_host_interface_claim(client, i->device->handle, i->info.number, 0);
    if (result != ESP_OK) {
        error(i, "interface claim", result);
        return false;
    }
    i->claimed = true;
    return true;
}
static void start_interface(kms_usb_interface_t *i)
{
    i->device->allocation_sequence = kms_usb_memory_sequence();
    esp_err_t result = usb_host_transfer_alloc(i->packet_bytes, 0, &i->transfer);
    if (result != ESP_OK) { error(i, "input memory", result); return; }
    usb_transfer_t *t = i->transfer;
    t->device_handle = i->device->handle; t->bEndpointAddress = i->endpoint;
    t->callback = report_done; t->context = i; t->num_bytes = i->packet_bytes;
    if (i->subclass == 1) (void)control(i, CONTROL_PROTOCOL);
    else activate(i);
}
static void process_control(device_t *d)
{
    if (!d->control_done) return;
    kms_usb_interface_t *i = d->control_interface;
    unsigned phase = d->control_phase;
    d->control_done = false; d->control_interface = NULL;
    if (!i || d->gone) return;
    usb_transfer_t *t = d->control;
    if (phase == CONTROL_HEALTH) {
        d->last_probe = kms_now();
        d->probe_ok = t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes == t->num_bytes;
        if (d->probe_ok) d->probe_failures = 0;
        else if (++d->probe_failures >= KMS_USB_PROBE_FAILURES)
            kms_usb_health_fault(KMS_USB_PROBE, d->info.address, i->info.number);
        return;
    }
    if (t->status != USB_TRANSFER_STATUS_COMPLETED || t->actual_num_bytes != t->num_bytes) {
        error(i, phase == CONTROL_REPORT ? "report descriptor transfer" : "report protocol",
              ESP_ERR_INVALID_RESPONSE); return;
    }
    if (phase == CONTROL_PROTOCOL) { activate(i); return; }
    i->probed = true; plan_dirty = true;
    // Selection already uses the kinds cached by the initial discovery pass.
    // Reserve aligned HCD descriptor lists before retaining a large HID parser:
    // otherwise that parser can consume the last suitably aligned DMA blocks.
    // Keep the descriptor response in EP0 until parsing and startup complete.
    bool selected = i->info.wanted && i->selected;
    if (selected && !claim(i)) return;
    d->allocation_sequence = kms_usb_memory_sequence();
    // Parsing needs a 7,780-byte workspace but never uses DMA. Keep this
    // temporary allocation out of the fragmented internal USB heap when
    // PSRAM is available. kms_hid_parse initializes the entire workspace.
    kms_hid_parser_t *parser = heap_caps_malloc(sizeof(*parser), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!parser) parser = calloc(1, sizeof(*parser));
    if (!parser) { error(i, "parser memory", ESP_ERR_NO_MEM); return; }
    bool supported = kms_hid_parse(parser, t->data_buffer + USB_SETUP_PACKET_SIZE, i->info.descriptor_bytes);
    i->info.kinds = supported ? kms_usb_hid_kinds(parser) : 0;
    if (!supported || !i->info.kinds) {
        free(parser); stop(i, KMS_USB_UNSUPPORTED); return;
    }
    if (!i->info.wanted || !i->selected) {
        free(parser); i->info.state = i->info.wanted ? KMS_USB_STANDBY : KMS_USB_OFF; return;
    }
    // Full parser workspaces are 7,780 bytes, while common receivers use only
    // 2-7 input fields. Retaining all 64 fields fragments the DMA/default heap
    // and can prevent either the next aligned pipe or parser from allocating.
    // No callback has seen this parser yet. Shrink it before publishing it;
    // a failed shrink leaves the original allocation valid. Ordinary realloc
    // requests internal RAM with CAPS_ALLOC, moving PSRAM parsers into the USB
    // heap. Request only 8-bit capability so either existing heap stays valid.
    kms_hid_parser_t *retained = heap_caps_realloc(parser, kms_hid_parser_storage_size(parser), MALLOC_CAP_8BIT);
    i->parser = retained ? retained : parser; start_interface(i);
}
static void parse_interfaces(device_t *d, const usb_config_desc_t *config)
{
    const uint8_t *bytes = (const uint8_t *)config;
    kms_usb_interface_t *current = NULL;
    for (unsigned at = 0; at + 2 <= config->wTotalLength;) {
        unsigned length = bytes[at], type = bytes[at + 1];
        if (length < 2 || length > config->wTotalLength - at) break;
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            current = NULL;
            if (length >= sizeof(usb_intf_desc_t)) {
                const usb_intf_desc_t *desc = (const usb_intf_desc_t *)(bytes + at);
                if (desc->bAlternateSetting == 0) {
                    if (desc->bInterfaceClass == HUB_CLASS) d->info.hub_endpoints += desc->bNumEndpoints;
                    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) if (!interfaces[n].device) {
                        current = &interfaces[n];
                        current->device = d; current->subclass = desc->bInterfaceSubClass;
                        current->info = (kms_usb_interface_info_t){.address = d->info.address,
                            .number = desc->bInterfaceNumber, .device_class = desc->bInterfaceClass,
                            .endpoints = desc->bNumEndpoints,
                            .state = desc->bInterfaceClass == HID_CLASS ? KMS_USB_PROBING : KMS_USB_IGNORED};
                        kms_usb_rule_t identity = key(current);
                        current->info.wanted = desc->bInterfaceClass == HID_CLASS && kms_usb_policy_enabled(&policy, &identity);
                        break;
                    }
                }
            }
        } else if (current && type == HID_DESCRIPTOR && length >= 6 && current->info.device_class == HID_CLASS) {
            for (unsigned n = 0; n < bytes[at + 5] && 6 + n * 3 + 3 <= length; ++n) {
                unsigned pos = at + 6 + n * 3;
                if (bytes[pos] == REPORT_DESCRIPTOR)
                    current->info.descriptor_bytes = bytes[pos + 1] | (uint16_t)bytes[pos + 2] << 8;
            }
        } else if (current && type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && length >= sizeof(usb_ep_desc_t)) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)(bytes + at);
            if (!current->endpoint && USB_EP_DESC_GET_EP_DIR(ep) && USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_INTR) {
                current->endpoint = ep->bEndpointAddress; current->packet_bytes = USB_EP_DESC_GET_MPS(ep);
            }
        }
        at += length;
    }
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        kms_usb_interface_t *i = &interfaces[n];
        if (i->device == d && i->info.device_class == HID_CLASS &&
            (!i->endpoint || !i->packet_bytes || i->packet_bytes > KMS_REPORT_BYTES ||
             !i->info.descriptor_bytes || i->info.descriptor_bytes > KMS_DESCRIPTOR_BYTES)) {
            i->probed = true; i->info.state = KMS_USB_UNSUPPORTED;
        }
    }
}
static uint32_t serial_hash(const usb_str_desc_t *s)
{
    if (!s || s->bLength <= 2) return 0;
    uint32_t hash = 2166136261u;
    for (unsigned n = 0; n < (s->bLength - 2u) / 2u; ++n) {
        hash = (hash ^ (s->wData[n] & 255)) * 16777619u;
        hash = (hash ^ (s->wData[n] >> 8)) * 16777619u;
    }
    return hash ? hash : 1;
}
static unsigned hub_endpoints(const usb_config_desc_t *config)
{
    const uint8_t *bytes = (const uint8_t *)config;
    unsigned endpoints = 0;
    for (unsigned at = 0; at + 2 <= config->wTotalLength;) {
        unsigned length = bytes[at], type = bytes[at + 1];
        if (length < 2 || length > config->wTotalLength - at) break;
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && length >= sizeof(usb_intf_desc_t)) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)(bytes + at);
            if (!i->bAlternateSetting && i->bInterfaceClass == HUB_CLASS) endpoints += i->bNumEndpoints;
        }
        at += length;
    }
    return endpoints;
}
static void open_device(uint8_t address)
{
    for (unsigned n = 0; n < hub_count; ++n) if (hubs[n].address == address) return;
    device_t *d = NULL;
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n) {
        if (devices[n].handle && devices[n].info.address == address && !devices[n].gone) return;
        if (!devices[n].handle && !d) d = &devices[n];
    }
    if (!d || usb_host_device_open(client, address, &d->handle) != ESP_OK) return;
    const usb_device_desc_t *desc;
    const usb_config_desc_t *config;
    usb_device_info_t info;
    if (usb_host_get_device_descriptor(d->handle, &desc) != ESP_OK ||
        usb_host_get_active_config_descriptor(d->handle, &config) != ESP_OK ||
        usb_host_device_info(d->handle, &info) != ESP_OK) { d->gone = true; return; }
    d->info = (kms_usb_device_info_t){.address = address, .vid = desc->idVendor, .pid = desc->idProduct,
        .serial = serial_hash(info.str_desc_serial_num), .port = info.parent.port_num, .speed = info.speed};
    if (info.parent.dev_hdl) {
        usb_device_info_t parent;
        if (usb_host_device_info(info.parent.dev_hdl, &parent) == ESP_OK) d->info.parent = parent.dev_addr;
    }
    if (info.str_desc_product && info.str_desc_product->bLength >= 2)
        for (unsigned n = 0; n < (info.str_desc_product->bLength - 2u) / 2u && n < sizeof(d->info.name) - 1; ++n) {
            uint16_t ch = info.str_desc_product->wData[n];
            d->info.name[n] = ch >= 32 && ch < 127 ? ch : '?';
        }
    d->info.hub_endpoints = hub_endpoints(config);
    if (d->info.hub_endpoints) {
        // The SDK's internal hub driver suppresses NEW_DEV and DEV_GONE for
        // hubs. Cache metadata only: retaining our own reference would prevent
        // their destruction, since we would never receive their disconnect.
        if (hub_count < KMS_USB_DEVICES) hubs[hub_count++] = d->info;
        (void)usb_host_device_close(client, d->handle);
        memset(d, 0, sizeof(*d)); plan_dirty = true; return;
    }
    parse_interfaces(d, config); plan_dirty = true;
    ESP_LOGI(TAG, "USB %u %04x:%04x parent=%u port=%u speed=%u %s",
             address, d->info.vid, d->info.pid, d->info.parent, d->info.port, d->info.speed, d->info.name);
}
static void discover_devices(void)
{
    uint8_t addresses[KMS_USB_DEVICES]; int count;
    if (usb_host_device_addr_list_fill(KMS_USB_DEVICES, addresses, &count) != ESP_OK) return;
    for (unsigned n = 0; n < hub_count;) {
        bool present = false;
        for (int j = 0; j < count; ++j) if (addresses[j] == hubs[n].address) present = true;
        if (present) { ++n; continue; }
        hubs[n] = hubs[--hub_count]; plan_dirty = true;
    }
    for (int n = 0; n < count; ++n) open_device(addresses[n]);
}
static void close_gone(device_t *d)
{
    bool drained = !d->control_busy && !d->control_done;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        kms_usb_interface_t *i = &interfaces[n];
        if (i->device != d) continue;
        stop(i, KMS_USB_REMOVING);
        if (!finish_stop(i)) drained = false;
    }
    if (!drained) return;
    if (d->control) {
        if (usb_host_transfer_free(d->control) != ESP_OK) return;
        d->control = NULL;
    }
    if (usb_host_device_close(client, d->handle) != ESP_OK) return;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].device == d) memset(&interfaces[n], 0, sizeof(interfaces[n]));
    memset(d, 0, sizeof(*d)); plan_dirty = true;
}
void kms_usb_devices_init(const kms_usb_input_callbacks_t *callbacks)
{
    kms_usb_memory_init();
    input = *callbacks; plan_dirty = true; quiescing = false;
    raw_head = raw_count = next_delay = 0; raw_overflow = false; next_generation = 0; last_dispatch_end = 0;
    memset(&stream, 0, sizeof(stream));
    kms_usb_preferences_init(&policy);
    usb_host_config_t config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&config));
    usb_host_client_config_t c = {.is_synchronous = false, .max_num_event_msg = KMS_USB_EVENT_DEPTH,
        .async = {.client_event_callback = client_event}};
    ESP_ERROR_CHECK(usb_host_client_register(&c, &client));
    configASSERT(xTaskCreate(daemon, "usb-host", KMS_USB_TASK_STACK, NULL, KMS_USB_TASK_PRIORITY, NULL) == pdPASS);
}
void kms_usb_devices_tick(void)
{
    int64_t started = esp_timer_get_time();
    uint32_t gap_us = last_dispatch_end ? timing(&stream.poll_gap, last_dispatch_end) : 0;
    (void)usb_host_client_handle_events(client, 0);
    uint32_t dispatch_us = timing(&stream.dispatch, started); last_dispatch_end = esp_timer_get_time();
    if (gap_us >= 5000 || dispatch_us >= 2000) {
        stream.recent[next_delay] = (kms_usb_delay_t){.at = kms_now(), .gap_us = gap_us, .dispatch_us = dispatch_us};
        next_delay = (next_delay + 1) % KMS_USB_STREAM_DELAYS;
        if (stream.recent_count < KMS_USB_STREAM_DELAYS) ++stream.recent_count;
    }
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n) if (devices[n].handle) {
        process_control(&devices[n]);
        if (devices[n].gone) close_gone(&devices[n]);
        else if (devices[n].retrying && retry_ready(&devices[n])) {
            devices[n].retrying = false; plan_dirty = true;
        }
    }
    if (quiescing) {
        for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
            if (interfaces[n].device && interfaces[n].stopping) (void)finish_stop(&interfaces[n]);
        stream.stale += raw_count; raw_head = raw_count = 0; raw_overflow = false; return;
    }
    for (unsigned address = 1; address < 128; ++address) if (pending[address]) {
        pending[address] = false; open_device(address);
    }
    discover_devices();
    reconcile_selection();
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        kms_usb_interface_t *i = &interfaces[n];
        if (!i->device || i->device->gone) continue;
        if (i->stopping) { (void)finish_stop(i); continue; }
        if (i->device->retrying) continue;
        if (i->completed) {
            i->completed = false;
            if (i->active && i->submit_result != ESP_OK) error(i, "input submission", i->submit_result);
            else if (i->active && i->transfer->status != USB_TRANSFER_STATUS_COMPLETED) error(i, "input transfer", ESP_FAIL);
        }
        if (i->active && !i->inflight) {
            (void)submit_report(i);
        }
        if (i->stopping || i->device->retrying) continue;
        device_t *d = i->device;
        if (i->selected && i->info.wanted && !i->claimed && can_claim_device(i) &&
            (i->info.state == KMS_USB_LIMIT || i->info.state == KMS_USB_STANDBY) && channels_used() + i->info.endpoints <= KMS_USB_CHANNELS) {
            i->probed = false; i->info.state = KMS_USB_PROBING;
        }
        if (!i->probed && i->info.device_class == HID_CLASS && !i->claimed && !d->control_busy && !d->control_done)
            (void)control(i, CONTROL_REPORT);
        // A standard device GET_STATUS checks bus liveness even when interrupt
        // inputs are idle and keyboard LED forwarding is disabled. Reuse EP0.
        if (i->active && !d->control_busy && !d->control_done &&
            kms_time_elapsed(kms_now(), d->last_probe, KMS_USB_PROBE_MS)) {
            d->last_probe = kms_now();
            if (!control(i, CONTROL_HEALTH)) kms_usb_health_fault(KMS_USB_PROBE, d->info.address, i->info.number);
        }
    }
    process_reports();
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n) {
        device_t *d = &devices[n];
        if (d->control_busy && !d->control_warned && kms_time_elapsed(kms_now(), d->submitted, 1500)) {
            d->control_warned = true;
            if (d->control_interface) d->control_interface->info.state = KMS_USB_ERROR;
            ESP_LOGW(TAG, "USB %u control transfer pending; keeping its buffer until completion or unplug", d->info.address);
        }
    }
}
void kms_usb_devices_quiesce(bool value)
{
    quiescing = value;
    if (!value) { memset(pending, 0, sizeof(pending)); hub_count = 0; plan_dirty = true; return; }
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].device) stop(&interfaces[n], KMS_USB_REMOVING);
}
bool kms_usb_devices_drained(void)
{
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n) if (devices[n].handle) return false;
    return true;
}
unsigned kms_usb_devices_active(void)
{
    unsigned count = 0;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) count += interfaces[n].active;
    return count;
}
unsigned kms_usb_devices_healthy(void)
{
    unsigned count = 0;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) if (interfaces[n].active) {
        if (!interfaces[n].device->probe_ok) return 0;
        ++count;
    }
    return count;
}
void kms_usb_devices_fault_snapshot(kms_usb_inventory_t *s)
{
    memset(s, 0, sizeof(*s)); s->automatic = policy.automatic;
    s->stream = stream; s->stream.pending = raw_count;
    unsigned oldest = (next_delay + KMS_USB_STREAM_DELAYS - stream.recent_count) % KMS_USB_STREAM_DELAYS;
    for (unsigned n = 0; n < stream.recent_count; ++n)
        s->stream.recent[n] = stream.recent[(oldest + n) % KMS_USB_STREAM_DELAYS];
    for (unsigned n = 0; n < hub_count; ++n) {
        s->devices[s->device_count++] = hubs[n];
        s->hubs += hubs[n].hub_endpoints;
    }
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n)
        if (devices[n].handle && !devices[n].gone && s->device_count < KMS_USB_DEVICES)
            s->devices[s->device_count++] = devices[n].info;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) if (interfaces[n].device) {
        if (!interfaces[n].device->gone) {
            kms_usb_interface_info_t *out = &s->interfaces[s->interface_count++];
            *out = interfaces[n].info; out->endpoint = interfaces[n].endpoint;
            out->transfer_pending = interfaces[n].inflight;
        }
        if (interfaces[n].claimed) s->inputs += interfaces[n].info.endpoints;
    }
}
void kms_usb_devices_snapshot(kms_usb_inventory_t *s)
{
    kms_usb_devices_fault_snapshot(s);
    usb_host_lib_info_t host;
    if (usb_host_lib_info(&host) == ESP_OK) s->controls = host.num_devices;
}
static void apply_policy(void)
{
    plan_dirty = true;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        kms_usb_interface_t *i = &interfaces[n];
        if (!i->device || i->device->gone || i->info.device_class != HID_CLASS) continue;
        kms_usb_rule_t identity = key(i);
        i->info.wanted = kms_usb_policy_enabled(&policy, &identity);
        if (!i->info.wanted) stop(i, i->probed && !i->info.kinds ? KMS_USB_UNSUPPORTED : KMS_USB_OFF);
        // Policy changes do not change a parsed descriptor. Re-probing an
        // unsupported sibling would temporarily invalidate its whole receiver
        // and retire otherwise working keyboard/pointer endpoints.
        else if (!i->active && i->info.state != KMS_USB_UNSUPPORTED) {
            i->probed = false; i->info.state = KMS_USB_PROBING;
        }
    }
    if (!kms_usb_preferences_save(&policy))
        ESP_LOGW(TAG, "USB choices apply now; saving unavailable");
}
bool kms_usb_devices_matches(const kms_usb_device_info_t *expected)
{
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n) {
        const device_t *device = &devices[n];
        const kms_usb_device_info_t *actual = &device->info;
        if (device->handle && !device->gone && actual->address == expected->address)
            return actual->vid == expected->vid && actual->pid == expected->pid && actual->serial == expected->serial &&
                actual->parent == expected->parent && actual->port == expected->port;
    }
    return false;
}
bool kms_usb_devices_select(unsigned address, unsigned number, bool enabled)
{
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        kms_usb_interface_t *i = &interfaces[n];
        if (!i->device || i->device->gone || i->info.device_class != HID_CLASS ||
            i->info.address != address || i->info.number != number) continue;
        kms_usb_policy_t changed = policy;
        for (unsigned j = 0; j < KMS_USB_INTERFACES; ++j) {
            const kms_usb_interface_t *other = &interfaces[j];
            if (other->device != i->device || other->info.device_class != HID_CLASS) continue;
            kms_usb_rule_t rule = key(other); rule.enabled = enabled;
            if (!kms_usb_policy_set(&changed, rule)) return false;
        }
        policy = changed; apply_policy(); return true;
    }
    return false;
}
bool kms_usb_devices_automatic(bool enabled)
{
    kms_usb_policy_reset(&policy, enabled);
    if (!enabled) for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) if (interfaces[n].active) {
        kms_usb_rule_t rule = key(&interfaces[n]); rule.enabled = 1;
        (void)kms_usb_policy_set(&policy, rule);
    }
    apply_policy(); return true;
}
void kms_usb_devices_rescan(void)
{
    plan_dirty = true;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) {
        kms_usb_interface_t *i = &interfaces[n];
        if (i->device && !i->device->gone && !i->active && i->info.device_class == HID_CLASS &&
            i->endpoint && i->packet_bytes && i->packet_bytes <= KMS_REPORT_BYTES &&
            i->info.descriptor_bytes && i->info.descriptor_bytes <= KMS_DESCRIPTOR_BYTES) {
            i->probed = false; i->info.state = KMS_USB_PROBING;
        }
    }
}
