#include "usb_leds.h"
#include "usb_health.h"
#include "kms_runtime.h"
#include "kms_diagnostic_store.h"
#include "usb_input.h"
#include <string.h>
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid.h"

typedef struct {
    usb_device_handle_t handle;
    uint8_t address;
    unsigned refs;
    bool gone, busy;
} device_t;
typedef struct {
    void *owner;
    device_t *device;
    const kms_hid_parser_t *parser;
    usb_transfer_t *transfer;
    uint8_t interface_number, report_id, cursor, pending[32], failures[256];
    uint32_t revision, sent_revision, refreshed, submitted, retry_since;
    unsigned completed, failed;
    bool busy, retiring, warned, retry_wait;
} output_t;
static device_t devices[KMS_USB_HID_INTERFACES];
static output_t outputs[KMS_USB_HID_INTERFACES];
static usb_host_client_handle_t client;
static bool desired_known;
static bool quiescing;
static uint8_t desired;
static const char *TAG = "usb-leds";

static void dirty(output_t *o, uint32_t now)
{
    ++o->revision; o->refreshed = now; o->retry_wait = false;
    memset(o->failures, 0, sizeof(o->failures)); memset(o->pending, 0, sizeof(o->pending));
    for (unsigned id = 0; id < 256; ++id) if (kms_hid_led_report_supported(o->parser, id))
        o->pending[id / 8] |= 1u << (id % 8);
}
static void retire(output_t *o) { o->retiring = true; o->parser = NULL; }
static void client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event != USB_HOST_CLIENT_EVENT_DEV_GONE) return;
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) {
        output_t *o = &outputs[n];
        if (o->owner && o->device->handle == event->dev_gone.dev_hdl) {
            o->device->gone = true; retire(o);
        }
    }
}
static void failed(output_t *o, uint32_t now)
{
    ++o->failed; o->retry_wait = true; o->retry_since = now;
    if (o->sent_revision != o->revision) return;
    if (++o->failures[o->report_id] >= KMS_LED_USB_ATTEMPTS) {
        kms_usb_input_event(KMS_EVENT_LED_FAILED, kms_usb_input_selected(), o->device->address);
        o->pending[o->report_id / 8] &= (uint8_t)~(1u << (o->report_id % 8));
        ESP_LOGW(TAG, "LED report %u on %u/%u failed; retry on refresh", o->report_id,
                 o->device->address, o->interface_number);
    }
}
static void completed(usb_transfer_t *transfer)
{
    output_t *o = transfer->context;
    kms_usb_health_complete(o, transfer->status, transfer->actual_num_bytes, transfer->num_bytes);
    o->busy = o->device->busy = false;
    if (o->retiring) return;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes == transfer->num_bytes) {
        ++o->completed; o->retry_wait = false;
        if (o->sent_revision == o->revision)
            o->pending[o->report_id / 8] &= (uint8_t)~(1u << (o->report_id % 8));
    } else failed(o, kms_now());
}
static bool enabled = KMS_KEYBOARD_LEDS_ENABLED;
void kms_usb_leds_set_enabled(bool value)
{
    if (enabled == value) return;
    enabled = value;
    if (enabled) for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n)
        if (outputs[n].owner && !outputs[n].retiring) dirty(&outputs[n], kms_now());
}
void kms_usb_leds_init(void)
{
    quiescing = false;
    // Register even when disabled so feedback can be enabled without re-enumeration.
    usb_host_client_config_t c = {.is_synchronous = false, .max_num_event_msg = KMS_USB_EVENT_DEPTH,
                                 .async = {.client_event_callback = client_event}};
    ESP_ERROR_CHECK(usb_host_client_register(&c, &client));
}
void kms_usb_leds_attach(void *owner, uint8_t address, uint8_t interface_number, const kms_hid_parser_t *parser)
{
    if (!client || !owner) return;
    bool supported = false;
    for (unsigned id = 0; id < 256; ++id) supported |= kms_hid_led_report_supported(parser, id);
    if (!supported) { ESP_LOGI(TAG, "No safe LED output on %u/%u", address, interface_number); return; }
    output_t *o = NULL; device_t *d = NULL, *free_device = NULL;
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) {
        if (!outputs[n].owner && !o) o = &outputs[n];
        if (devices[n].handle && !devices[n].gone && devices[n].address == address) d = &devices[n];
        if (!devices[n].handle && !free_device) free_device = &devices[n];
    }
    if (!o || (!d && !free_device)) { ESP_LOGW(TAG, "LED output capacity reached"); return; }
    usb_transfer_t *transfer = NULL;
    if (usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + KMS_REPORT_BYTES, 0, &transfer) != ESP_OK) {
        ESP_LOGW(TAG, "No transfer memory for keyboard LEDs"); return;
    }
    if (!d) {
        d = free_device;
        if (usb_host_device_open(client, address, &d->handle) != ESP_OK) {
            usb_host_transfer_free(transfer); memset(d, 0, sizeof(*d)); return;
        }
        d->address = address;
    }
    ++d->refs;
    *o = (output_t){.owner = owner, .device = d, .parser = parser,
                    .transfer = transfer, .interface_number = interface_number};
    dirty(o, kms_now());
    ESP_LOGI(TAG, "Keyboard LED output ready on %u/%u (%u fields)", address, interface_number, parser->led_count);
}
void kms_usb_leds_detach(void *owner)
{
    if (!owner) return;
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n)
        if (outputs[n].owner == owner) retire(&outputs[n]);
}
static void cleanup(output_t *o)
{
    // A removed interface's parser may already be freed; callbacks use only o.
    // Keep the transfer and the device reference until completion/cancellation.
    device_t *d = o->device;
    ESP_ERROR_CHECK(usb_host_transfer_free(o->transfer)); memset(o, 0, sizeof(*o));
    if (!--d->refs) { ESP_ERROR_CHECK(usb_host_device_close(client, d->handle)); memset(d, 0, sizeof(*d)); }
}
void kms_usb_leds_tick(bool known, uint8_t leds)
{
    if (!client) return;
    (void)usb_host_client_handle_events(client, 0);
    uint32_t now = kms_now();
    leds = known ? leds & KMS_LOCK_MASK : 0;
    bool changed = desired_known != known || desired != leds;
    desired_known = known; desired = leds;
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) {
        output_t *o = &outputs[n];
        if (!o->owner) continue;
        if (o->retiring) { if (!o->busy) cleanup(o); continue; }
        if (changed || kms_time_elapsed(now, o->refreshed, KMS_LED_USB_REFRESH_MS)) dirty(o, now);
        if (o->busy) {
            // This SDK does not implement transfer.timeout_ms. Never reuse/free an
            // in-flight buffer or reset shared EP0 to fix an LED: input has priority.
            if (!o->warned && kms_time_elapsed(now, o->submitted, KMS_LED_USB_WATCHDOG_MS)) {
                kms_usb_input_event(KMS_EVENT_LED_PENDING, kms_usb_input_selected(), o->device->address);
                ESP_LOGW(TAG, "LED transfer pending on %u/%u; waiting asynchronously", o->device->address, o->interface_number);
                o->warned = true;
            }
            continue;
        }
        if (!enabled || quiescing) continue;
        if (o->device->busy || (o->retry_wait && !kms_time_elapsed(now, o->retry_since, KMS_LED_USB_RETRY_MS))) continue;
        for (unsigned j = 0; j < 256; ++j) {
            uint8_t id = o->cursor++;
            if (!(o->pending[id / 8] & (1u << (id % 8)))) continue;
            usb_transfer_t *t = o->transfer;
            size_t bytes = kms_hid_led_report(o->parser, id, desired, t->data_buffer + USB_SETUP_PACKET_SIZE, KMS_REPORT_BYTES);
            if (!bytes) { o->pending[id / 8] &= (uint8_t)~(1u << (id % 8)); continue; }
            usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
            *s = (usb_setup_packet_t){.bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                .bRequest = HID_CLASS_SPECIFIC_REQ_SET_REPORT, .wValue = (HID_REPORT_TYPE_OUTPUT << 8) | id,
                .wIndex = o->interface_number, .wLength = bytes};
            t->device_handle = o->device->handle; t->bEndpointAddress = 0;
            t->callback = completed; t->context = o; t->num_bytes = USB_SETUP_PACKET_SIZE + bytes;
            o->report_id = id; o->sent_revision = o->revision; o->submitted = now; o->warned = false;
            esp_err_t e = usb_host_transfer_submit_control(client, t);
            if (e == ESP_OK) {
                o->busy = o->device->busy = true;
                kms_usb_health_begin(o, KMS_USB_LED, o->device->address, o->interface_number);
                kms_usb_health_request(o, s, t->num_bytes);
            }
            else {
                kms_usb_health_rejected(KMS_USB_LED, o->device->address, o->interface_number, s, e);
                failed(o, now);
            }
            break; // One transfer at a time per interface and physical device.
        }
    }
}
void kms_usb_leds_quiesce(bool value)
{
    quiescing = value;
    if (value) for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n)
        if (outputs[n].owner) retire(&outputs[n]);
}
bool kms_usb_leds_drained(void)
{
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) if (outputs[n].owner) return false;
    return true;
}
void kms_usb_leds_status(void)
{
    kms_usb_led_status_t snapshot;
    kms_usb_leds_snapshot(&snapshot); kms_usb_leds_print_status(&snapshot);
}
void kms_usb_leds_snapshot(kms_usb_led_status_t *out)
{
    memset(out, 0, sizeof(*out)); out->enabled = enabled; out->known = desired_known; out->desired = desired;
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) {
        const output_t *o = &outputs[n];
        if (!o->owner) continue;
        unsigned i = out->count++;
        out->outputs[i].address = o->device->address; out->outputs[i].number = o->interface_number;
        out->outputs[i].completed = o->completed; out->outputs[i].failed = o->failed;
        out->outputs[i].busy = o->busy; out->outputs[i].retiring = o->retiring;
    }
}
void kms_usb_leds_print_status(const kms_usb_led_status_t *snapshot)
{
    ESP_LOGI(TAG, "Keyboard LED forwarding %s; selected state %s mask=0x%02x",
             snapshot->enabled ? "enabled" : "disabled", snapshot->known ? "known" : "unknown", snapshot->desired);
    for (unsigned n = 0; n < snapshot->count; ++n) {
        ESP_LOGI(TAG, "%u/%u: sent=%u failed=%u pending=%s retiring=%s",
            snapshot->outputs[n].address, snapshot->outputs[n].number,
            snapshot->outputs[n].completed, snapshot->outputs[n].failed,
            snapshot->outputs[n].busy ? "yes" : "no", snapshot->outputs[n].retiring ? "yes" : "no");
    }
}
