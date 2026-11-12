// Exercise the production asynchronous sender against a fake USB host. No device I/O.
#include "usb_leds.h"
#include "usb_health.h"
#include "kms_diagnostic_store.h"
#include "usb/usb_host.h"
#include <string.h>

#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
typedef struct { usb_transfer_t t; uint8_t bytes[USB_SETUP_PACKET_SIZE + KMS_REPORT_BYTES]; bool used, inflight, finish; } fake_transfer_t;
static fake_transfer_t transfers[KMS_USB_HID_INTERFACES];
static unsigned checks, errors, opens, closes, submits, event_polls;
static uint32_t now;
static unsigned fake_devices[128];
static usb_host_client_config_t config;
static usb_device_handle_t gone;
static bool reject_submit, reject_alloc, reject_open;
uint32_t kms_now(void) { return now; }
unsigned kms_link_selected(void) { return 1; }
unsigned kms_usb_input_selected(void) { return kms_link_selected(); }
void kms_usb_input_event(enum kms_event e, unsigned slot, uint32_t detail)
{ kms_diagnostics_event(e, slot, detail); }
void kms_diagnostics_event(enum kms_event e, unsigned slot, uint32_t detail)
{ (void)e; (void)slot; (void)detail; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void kms_fake_check(int e) { if (e != ESP_OK) ++errors; }
esp_err_t usb_host_client_register(const usb_host_client_config_t *c, usb_host_client_handle_t *out)
{ config = *c; *out = &config; return ESP_OK; }
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t c, int timeout)
{
    (void)c; ++event_polls; if (timeout) ++errors;
    if (gone) {
        usb_host_client_event_msg_t event = {.event = USB_HOST_CLIENT_EVENT_DEV_GONE, .dev_gone = {.dev_hdl = gone}};
        gone = NULL; config.async.client_event_callback(&event, NULL);
    }
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) if (transfers[n].finish) {
        transfers[n].finish = transfers[n].inflight = false; transfers[n].t.callback(&transfers[n].t);
    }
    return ESP_OK;
}
esp_err_t usb_host_device_open(usb_host_client_handle_t c, uint8_t addr, usb_device_handle_t *out)
{
    (void)c; if (reject_open) return ESP_FAIL;
    if (fake_devices[addr]) ++errors;
    fake_devices[addr] = 1; *out = &fake_devices[addr]; ++opens; return ESP_OK;
}
esp_err_t usb_host_device_close(usb_host_client_handle_t c, usb_device_handle_t d)
{ (void)c; if (!*(unsigned *)d) ++errors; *(unsigned *)d = 0; ++closes; return ESP_OK; }
esp_err_t usb_host_transfer_alloc(size_t bytes, int packets, usb_transfer_t **out)
{
    (void)packets; if (reject_alloc) return ESP_FAIL;
    for (unsigned n = 0; n < KMS_USB_HID_INTERFACES; ++n) if (!transfers[n].used) {
        fake_transfer_t *f = &transfers[n]; memset(f, 0, sizeof(*f));
        if (bytes > sizeof(f->bytes)) { ++errors; return ESP_FAIL; }
        f->used = true; f->t.data_buffer = f->bytes; f->t.data_buffer_size = sizeof(f->bytes); *out = &f->t; return ESP_OK;
    }
    return ESP_FAIL;
}
esp_err_t usb_host_transfer_free(usb_transfer_t *t)
{
    fake_transfer_t *f = (fake_transfer_t *)t;
    if (f->inflight || !f->used) ++errors;
    f->used = false; return ESP_OK;
}
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t c, usb_transfer_t *t)
{
    (void)c; if (reject_submit) return ESP_FAIL;
    fake_transfer_t *f = (fake_transfer_t *)t;
    if (f->inflight || !f->used) ++errors;
    f->inflight = true; ++submits; return ESP_OK;
}
static void finish(unsigned n, bool success)
{
    transfers[n].t.status = success ? USB_TRANSFER_STATUS_COMPLETED : USB_TRANSFER_STATUS_ERROR;
    transfers[n].t.actual_num_bytes = transfers[n].t.num_bytes; transfers[n].finish = true;
}
static const uint8_t descriptor[] = {0x05,1,0x09,6,0xa1,1,0x05,8,0x15,0,0x25,1,0x19,1,0x29,5,
                                    0x75,1,0x95,5,0x91,2,0x75,3,0x95,1,0x91,1,0xc0};
__declspec(dllexport) int kms_run_usb_led_tests(void)
{
    static kms_hid_parser_t p; static int owners[4];
    kms_usb_health_init();
    CHECK(kms_hid_parse(&p, descriptor, sizeof(descriptor)));
    kms_usb_leds_init(); now = 10;
    kms_usb_leds_attach(&owners[0], 1, 0, &p); kms_usb_leds_attach(&owners[1], 1, 1, &p);
    CHECK(opens == 1); // Two HID interfaces share one additional device reference and EP0.
    kms_usb_leds_tick(true, KMS_LOCK_CAPS); CHECK(submits == 1 && event_polls == 1);
    CHECK(transfers[0].inflight && !transfers[1].inflight);
    usb_setup_packet_t *s = (usb_setup_packet_t *)transfers[0].bytes;
    CHECK(s->bmRequestType == 0x21 && s->bRequest == 9 && s->wValue == 0x200 && s->wIndex == 0 && s->wLength == 1);
    CHECK(transfers[0].bytes[8] == KMS_LOCK_CAPS);
    // Change destination/state while a report is in flight. Its completion must not clear the new work.
    kms_usb_leds_tick(true, KMS_LOCK_NUM); CHECK(submits == 1 && transfers[0].bytes[8] == KMS_LOCK_CAPS);
    finish(0, true); kms_usb_leds_tick(true, KMS_LOCK_NUM);
    CHECK(submits == 2 && transfers[0].bytes[8] == KMS_LOCK_NUM);
    finish(0, true); kms_usb_leds_tick(true, KMS_LOCK_NUM);
    CHECK(submits == 3 && transfers[1].inflight && transfers[1].bytes[8] == KMS_LOCK_NUM);
    finish(1, true); kms_usb_leds_tick(true, KMS_LOCK_NUM); CHECK(submits == 3);
    now += KMS_LED_USB_REFRESH_MS; kms_usb_leds_tick(true, KMS_LOCK_NUM); CHECK(submits == 4);
    finish(0, true); kms_usb_leds_tick(true, KMS_LOCK_NUM);
    finish(1, true); kms_usb_leds_tick(true, KMS_LOCK_NUM);
    unsigned before = submits;
    kms_usb_leds_tick(false, KMS_LOCK_CAPS); CHECK(submits == before + 1 && transfers[0].bytes[8] == 0);
    // Disconnect during an outstanding report: parser can go away; DMA storage must remain.
    kms_usb_leds_detach(&owners[0]); kms_usb_leds_tick(false, 0);
    CHECK(transfers[0].used && transfers[0].inflight && closes == 0);
    finish(0, true); kms_usb_leds_tick(false, 0);
    CHECK(!transfers[0].used && transfers[1].inflight && closes == 0);
    finish(1, true); kms_usb_leds_tick(false, 0);
    kms_usb_leds_detach(&owners[1]); kms_usb_leds_tick(false, 0); CHECK(closes == 1);
    kms_usb_leds_attach(&owners[2], 2, 3, &p); kms_usb_leds_tick(true, KMS_LOCK_SCROLL);
    before = submits; finish(0, false); kms_usb_leds_tick(true, KMS_LOCK_SCROLL); CHECK(submits == before);
    now += KMS_LED_USB_RETRY_MS - 1; kms_usb_leds_tick(true, KMS_LOCK_SCROLL); CHECK(submits == before);
    ++now; kms_usb_leds_tick(true, KMS_LOCK_SCROLL); CHECK(submits == before + 1);
    for (unsigned n = 1; n < KMS_LED_USB_ATTEMPTS; ++n) {
        finish(0, false); kms_usb_leds_tick(true, KMS_LOCK_SCROLL);
        now += KMS_LED_USB_RETRY_MS; kms_usb_leds_tick(true, KMS_LOCK_SCROLL);
    }
    CHECK(submits == before + KMS_LED_USB_ATTEMPTS - 1 && !transfers[0].inflight);
    now += KMS_LED_USB_REFRESH_MS; kms_usb_leds_tick(true, KMS_LOCK_SCROLL); CHECK(transfers[0].inflight);
    CHECK(kms_usb_health_pending() == 1);
    before = submits; now += KMS_LED_USB_WATCHDOG_MS; kms_usb_leds_tick(true, KMS_LOCK_SCROLL);
    CHECK(transfers[0].inflight && submits == before && errors == 0); // Watchdog never reuses DMA or resets shared EP0.
    kms_usb_overdue_t overdue;
    CHECK(!kms_usb_health_overdue(now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue));
    now += KMS_USB_CONTROL_TIMEOUT_MS;
    CHECK(kms_usb_health_overdue(now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue) && overdue.operation == KMS_USB_LED);
    kms_usb_leds_quiesce(true); CHECK(!kms_usb_leds_drained());
    gone = &fake_devices[2]; kms_usb_leds_tick(true, KMS_LOCK_SCROLL);
    CHECK(transfers[0].used && closes == 1);
    finish(0, false); kms_usb_leds_tick(true, KMS_LOCK_SCROLL); CHECK(!transfers[0].used && closes == 2);
    CHECK(kms_usb_leds_drained() && kms_usb_health_pending() == 0);
    kms_usb_leds_quiesce(false);
    // Allocation/open/submission failures remain optional LED failures, with no fatal check or leaks.
    reject_alloc = true; kms_usb_leds_attach(&owners[3], 3, 0, &p); CHECK(opens == 2); reject_alloc = false;
    reject_open = true; kms_usb_leds_attach(&owners[3], 3, 0, &p); CHECK(!transfers[0].used); reject_open = false;
    reject_submit = true; kms_usb_leds_attach(&owners[3], 3, 0, &p); kms_usb_leds_tick(true, 0);
    CHECK(!transfers[0].inflight); reject_submit = false;
    kms_usb_leds_detach(&owners[3]); kms_usb_leds_tick(true, 0); CHECK(!transfers[0].used && opens == closes && !errors);
    static const uint8_t numbered[] = {0x05,1,0x09,6,0xa1,1,0x85,9,0x05,8,0x15,0,0x25,1,0x19,1,0x29,5,
                                       0x75,1,0x95,5,0x91,2,0x75,3,0x95,1,0x91,1,0xc0};
    CHECK(kms_hid_parse(&p, numbered, sizeof(numbered)));
    now = 0xfffffff0u; kms_usb_leds_attach(&owners[0], 4, 4, &p); kms_usb_leds_tick(true, KMS_LOCK_CAPS);
    s = (usb_setup_packet_t *)transfers[0].bytes;
    CHECK(s->wValue == 0x209 && s->wIndex == 4 && s->wLength == 2);
    CHECK(transfers[0].bytes[8] == 9 && transfers[0].bytes[9] == KMS_LOCK_CAPS);
    now += KMS_LED_USB_WATCHDOG_MS; kms_usb_leds_tick(true, KMS_LOCK_CAPS); CHECK(transfers[0].inflight);
    kms_usb_leds_detach(&owners[0]); memset(&p, 0xff, sizeof(p)); // Parser memory is no longer usable.
    finish(0, true); kms_usb_leds_tick(true, KMS_LOCK_CAPS);
    CHECK(!transfers[0].used && opens == closes && !errors);
    CHECK(kms_hid_parse(&p, descriptor, sizeof(descriptor)));
    kms_usb_leds_set_enabled(false); before = submits;
    kms_usb_leds_attach(&owners[0], 5, 0, &p); kms_usb_leds_tick(true, KMS_LOCK_CAPS);
    CHECK(submits == before);
    kms_usb_leds_set_enabled(true); kms_usb_leds_tick(true, KMS_LOCK_CAPS);
    CHECK(submits == before + 1 && transfers[0].inflight);
    kms_usb_leds_set_enabled(false); finish(0, true); kms_usb_leds_tick(true, KMS_LOCK_NUM);
    CHECK(!transfers[0].inflight && submits == before + 1);
    kms_usb_leds_detach(&owners[0]); kms_usb_leds_tick(true, KMS_LOCK_NUM);
    CHECK(!transfers[0].used && opens == closes && !errors);
    kms_usb_leds_set_enabled(true);
    return 0;
}
__declspec(dllexport) int kms_usb_led_test_count(void) { return checks; }
