// USB Host Library 1.6.0 reproducer: no radio, router, HID parser, NVS writes,
// automatic recovery, application USB wrappers, or TinyUSB device stack.
#include "stream.h"
#ifdef ISO_DRIVER_TRACE
#include "driver_trace.h"
#include "frame_guard.h"
#endif
#include "pattern.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_intr_alloc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/usb_struct.h"
#include "usb/usb_host.h"

enum { SOURCES = 2, SAMPLES = 6, SAMPLE_MS = 500 };
enum { CMD_ARM, CMD_WATCH, CMD_STATUS, CMD_FAULT, CMD_WORKAROUND };
enum { FAULT_INPUT = 1, FAULT_CONTROL, FAULT_PAYLOAD, FAULT_GONE, FAULT_OPEN, FAULT_SUBMIT };
typedef struct {
    iso_stream_t stream;
    char serial[13];
    uint32_t address, last_report, usb_errors, pending, control_pending, control_start, controls;
    uint32_t submissions, callbacks, transfer;
} source_state_t;
typedef struct {
    uint32_t at, frame, port, core, mask, channels, ahb, clock;
} bus_sample_t;
typedef struct {
    uint32_t at, run, seed, profile, mode, watching, fault, expected, devices, frozen;
    source_state_t sources[SOURCES];
    bus_sample_t bus;
    bus_sample_t samples[SAMPLES];
    uint32_t samples_count;
    uint32_t reset, nptx, ptx, host, interval, frame_list, channel_mask;
    uint32_t channels[8][6];
#ifdef ISO_DRIVER_TRACE
    iso_guard_stats_t guard;
#endif
} snapshot_t;
typedef struct { unsigned kind, run, seed, profile, mode, expected, watching, workaround; } command_t;
typedef struct { unsigned result; snapshot_t snapshot; } reply_t;
typedef struct {
    usb_device_handle_t handle;
    usb_transfer_t *input, *control;
    iso_endpoint_t endpoint;
    source_state_t state;
    bool claimed, gone, resubmit;
    unsigned next_control;
    uint32_t last_control;
} device_t;

static usb_host_client_handle_t client;
static device_t devices[SOURCES];
static QueueHandle_t requests, replies;
static uint32_t run, seed, profile, mode, expected, watch_start, fault, boot;
static bool watching;
static char identity[13];
#ifdef ISO_DRIVER_TRACE
#define ISO_TRACE_IDENTITY " trace=1 workaround=1"
#else
#define ISO_TRACE_IDENTITY ""
#endif
static bus_sample_t samples[SAMPLES];
static unsigned sample_next, sample_count;
static uint32_t last_sample;
static snapshot_t frozen;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bus_sample_t bus_sample(void)
{
    // Read ordinary status registers only. Never pop RX data or acknowledge IRQs.
    return (bus_sample_t){now_ms(), USB0.hfnum, USB0.hprt, USB0.gintsts, USB0.gintmsk,
        USB0.haint, USB0.gahbcfg, *(volatile const uint32_t *)&USB0.pcgctrl};
}
static unsigned device_count(void)
{
    unsigned count = 0;
    for (unsigned n = 0; n < SOURCES; ++n) count += devices[n].handle && !devices[n].gone;
    return count;
}
static void snapshot(snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->at = now_ms(); out->run = run; out->seed = seed; out->profile = profile;
    out->mode = mode; out->watching = watching; out->fault = fault; out->expected = expected;
    out->devices = device_count();
#ifdef ISO_DRIVER_TRACE
    iso_guard_snapshot(&out->guard);
#endif
    out->bus = bus_sample();
    for (unsigned n = 0; n < SOURCES; ++n) out->sources[n] = devices[n].state;
    unsigned oldest = (sample_next + SAMPLES - sample_count) % SAMPLES;
    for (unsigned n = 0; n < sample_count; ++n) out->samples[n] = samples[(oldest + n) % SAMPLES];
    out->samples_count = sample_count;
    out->reset = USB0.grstctl; out->nptx = USB0.gnptxsts; out->ptx = USB0.hptxsts;
    out->host = USB0.hcfg; out->interval = USB0.hfir; out->frame_list = USB0.hflbaddr;
    out->channel_mask = USB0.haintmsk;
    for (unsigned n = 0; n < 8; ++n) {
        static const unsigned offsets[] = {0, 2, 3, 4, 5, 7};
        for (unsigned j = 0; j < 6; ++j) out->channels[n][j] = USB0.host_chan_regs[n * 8 + offsets[j]];
    }
}
static void freeze(unsigned code)
{
    if (!watching || fault) return;
    fault = code;
#ifdef ISO_DRIVER_TRACE
    iso_hcd_capture(true);
#endif
    snapshot(&frozen); frozen.frozen = 1;
    // Leave the SDK running and retain first-fault evidence. No reset or repair.
}
static void input_done(usb_transfer_t *transfer)
{
    device_t *d = transfer->context; d->state.pending = 0; ++d->state.callbacks;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        d->state.last_report = now_ms();
        if (run) iso_stream_report(&d->state.stream, transfer->data_buffer, (size_t)transfer->actual_num_bytes);
        if (d->state.stream.payload_errors || d->state.stream.sequence_errors) freeze(FAULT_PAYLOAD);
    } else if (!d->gone) { ++d->state.usb_errors; freeze(FAULT_INPUT); }
    d->resubmit = !d->gone;
}
static void control_done(usb_transfer_t *transfer)
{
    device_t *d = transfer->context; d->state.control_pending = 0;
    ++d->state.controls;
    if (!d->gone && (transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
                     transfer->actual_num_bytes != transfer->num_bytes)) {
        ++d->state.usb_errors; freeze(FAULT_CONTROL);
    }
}
static void submit_control(device_t *d, uint32_t now)
{
    bool led = mode == 2 || (mode == 3 && d->next_control++ % 2);
    usb_transfer_t *t = d->control;
    memset(t->data_buffer, 0, 10);
    usb_setup_packet_t *setup = (usb_setup_packet_t *)t->data_buffer;
    if (led) {
        *setup = (usb_setup_packet_t){.bmRequestType = 0x21, .bRequest = 9, .wValue = 0x0201,
            .wIndex = d->endpoint.interface, .wLength = 2};
        t->data_buffer[8] = 1; t->data_buffer[9] = (now / 1000) % 2 ? 2 : 0;
    } else {
        *setup = (usb_setup_packet_t){.bmRequestType = 0x80, .bRequest = 0, .wLength = 2};
    }
    t->num_bytes = 10; d->state.control_start = now; d->last_control = now;
    if (usb_host_transfer_submit_control(client, t) == ESP_OK) d->state.control_pending = 1;
    else { ++d->state.usb_errors; freeze(FAULT_SUBMIT); }
}
static void retire(device_t *d)
{
    if (!d->gone || d->state.pending || d->state.control_pending) return;
    // Cancellation callbacks must drain before any transfer or device is freed.
    if (d->claimed && usb_host_interface_release(client, d->handle, d->endpoint.interface) != ESP_OK) return;
    if (d->input) (void)usb_host_transfer_free(d->input);
    if (d->control) (void)usb_host_transfer_free(d->control);
    (void)usb_host_device_close(client, d->handle); memset(d, 0, sizeof(*d));
}
static void attach(uint8_t address)
{
    usb_device_handle_t handle;
    if (usb_host_device_open(client, address, &handle) != ESP_OK) { freeze(FAULT_OPEN); return; }
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(handle, &desc) != ESP_OK ||
        desc->idVendor != KMS_GEN_VID || desc->idProduct != KMS_GEN_PID) {
        (void)usb_host_device_close(client, handle); return;
    }
    device_t *d = NULL;
    for (unsigned n = 0; n < SOURCES; ++n) if (!devices[n].handle) { d = &devices[n]; break; }
    if (!d) { (void)usb_host_device_close(client, handle); freeze(FAULT_OPEN); return; }
    d->handle = handle; d->state.address = address;
    const usb_config_desc_t *config; usb_device_info_t info;
    if (usb_host_get_active_config_descriptor(handle, &config) != ESP_OK ||
        !iso_find_endpoint((const uint8_t *)config, config->wTotalLength, &d->endpoint) ||
        usb_host_device_info(handle, &info) != ESP_OK || !info.str_desc_serial_num ||
        info.str_desc_serial_num->bLength != 26 ||
        !iso_serial((const uint8_t *)info.str_desc_serial_num + 2, 24, d->state.serial) ||
        usb_host_transfer_alloc(d->endpoint.packet, 0, &d->input) != ESP_OK ||
        usb_host_transfer_alloc(10, 0, &d->control) != ESP_OK ||
        usb_host_interface_claim(client, handle, d->endpoint.interface, 0) != ESP_OK) {
        d->gone = true; retire(d); freeze(FAULT_OPEN); return;
    }
    d->claimed = true;
    d->state.transfer = (uint32_t)(uintptr_t)d->input;
    d->input->device_handle = handle; d->input->bEndpointAddress = d->endpoint.endpoint;
    d->input->num_bytes = d->endpoint.packet; d->input->callback = input_done; d->input->context = d;
    d->control->device_handle = handle; d->control->bEndpointAddress = 0;
    d->control->callback = control_done; d->control->context = d;
    d->state.stream = (iso_stream_t){.run = run, .seed = seed, .profile = profile};
    d->state.last_report = now_ms(); d->resubmit = true;
}
static void device_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) attach(event->new_dev.address);
    else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (unsigned n = 0; n < SOURCES; ++n) {
            device_t *d = &devices[n];
            if (!d->handle || d->handle != event->dev_gone.dev_hdl) continue;
            d->gone = true; d->resubmit = false; freeze(FAULT_GONE);
            if (d->claimed) {
                (void)usb_host_endpoint_halt(d->handle, d->endpoint.endpoint);
                (void)usb_host_endpoint_flush(d->handle, d->endpoint.endpoint);
            }
        }
    }
}
static void device_tick(device_t *d, uint32_t now)
{
    if (!d->handle) return;
    if (d->gone) { retire(d); return; }
    if (d->resubmit && !d->state.pending) {
        d->resubmit = false;
        if (usb_host_transfer_submit(d->input) == ESP_OK) { d->state.pending = 1; ++d->state.submissions; }
        else { ++d->state.usb_errors; freeze(FAULT_SUBMIT); }
    }
    if (!watching || fault) return;
    if (d->state.control_pending && iso_elapsed(now, d->state.control_start, 1000)) freeze(FAULT_CONTROL);
    uint32_t since = d->state.stream.count ? d->state.last_report : watch_start;
    if (iso_elapsed(now, since, d->state.stream.count ? 3000 : 10000)) freeze(FAULT_INPUT);
    if (fault) return;
    if (mode && !d->state.control_pending && iso_elapsed(now, d->last_control, mode == 3 ? 500 : 1000))
        submit_control(d, now);
}
static unsigned execute(command_t command)
{
    if (command.kind == CMD_ARM) {
        if (watching || !command.run || command.profile > 4 || command.mode > 3 ||
            !command.expected || command.expected > SOURCES || device_count() != command.expected) return 1;
        for (unsigned n = 0; n < SOURCES; ++n) if (devices[n].state.control_pending) return 2;
        run = command.run; seed = command.seed; profile = command.profile;
        mode = command.mode; expected = command.expected; fault = 0;
        sample_count = sample_next = 0; memset(&frozen, 0, sizeof(frozen));
#ifdef ISO_DRIVER_TRACE
        iso_driver_reset();
        iso_guard_reset();
#endif
        for (unsigned n = 0; n < SOURCES; ++n) {
            device_t *d = &devices[n];
            d->state.stream = (iso_stream_t){.run = run, .seed = seed, .profile = profile};
            d->state.usb_errors = d->state.controls = 0;
            d->state.submissions = d->state.callbacks = 0;
            d->last_control = now_ms(); d->next_control = 0;
        }
    } else if (command.kind == CMD_WATCH) {
        if (command.watching > 1 || (command.watching && (!run || fault || device_count() != expected))) return 3;
        watching = command.watching != 0; watch_start = now_ms();
    } else if (command.kind == CMD_WORKAROUND) {
#ifdef ISO_DRIVER_TRACE
        if (watching || command.workaround > 1) return 4;
        iso_guard_configure(command.workaround != 0);
#else
        return 4;
#endif
    }
    return 0;
}
static void usb_daemon(void *arg)
{
    (void)arg;
    while (true) { uint32_t flags; ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &flags)); }
}
static void worker(void *arg)
{
    (void)arg;
    usb_host_config_t config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&config));
    usb_host_client_config_t cc = {.is_synchronous = false, .max_num_event_msg = 8,
        .async = {.client_event_callback = device_event}};
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &client));
    configASSERT(xTaskCreatePinnedToCore(usb_daemon, "iso-host", 6144, NULL, 5, NULL, 1) == pdPASS);
    while (true) {
        ESP_ERROR_CHECK(usb_host_client_handle_events(client, 0));
        uint32_t now = now_ms();
        if (!sample_count || iso_elapsed(now, last_sample, SAMPLE_MS)) {
#ifdef ISO_DRIVER_TRACE
            iso_hcd_capture(false);
#endif
            samples[sample_next] = bus_sample(); sample_next = (sample_next + 1) % SAMPLES;
            if (sample_count < SAMPLES) ++sample_count;
            last_sample = now;
        }
        for (unsigned n = 0; n < SOURCES; ++n) device_tick(&devices[n], now);
        command_t command;
        if (xQueueReceive(requests, &command, 0) == pdTRUE) {
            reply_t reply = {.result = execute(command)};
            if (command.kind == CMD_FAULT && fault) reply.snapshot = frozen;
            else snapshot(&reply.snapshot);
            configASSERT(xQueueSend(replies, &reply, 0) == pdTRUE);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
static void print_snapshot(unsigned token, const snapshot_t *s, bool detail)
{
    printf("KMSISO BEGIN id=%u boot=%08" PRIx32 " at=%" PRIu32 " run=%" PRIu32
           " seed=%" PRIu32 " profile=%" PRIu32 " mode=%" PRIu32 " watching=%" PRIu32
           " fault=%" PRIu32 " expected=%" PRIu32 " devices=%" PRIu32 " frozen=%" PRIu32 "\n",
           token, boot, s->at, s->run, s->seed, s->profile, s->mode, s->watching,
           s->fault, s->expected, s->devices, s->frozen);
#ifdef ISO_DRIVER_TRACE
    printf("KMSISO WORKAROUND version=1 enabled=%" PRIu32 " guard-us=%u budget-us=%u"
           " activations=%" PRIu32 " waited=%" PRIu32 " timeouts=%" PRIu32
           " max-wait-us=%" PRIu32 " crossings=%" PRIu32 "\n", s->guard.enabled,
           ISO_GUARD_US, ISO_GUARD_BUDGET_US, s->guard.activations, s->guard.waited,
           s->guard.timeouts, s->guard.max_wait_us, s->guard.crossings);
#endif
    for (unsigned n = 0; n < SOURCES; ++n) {
        const source_state_t *d = &s->sources[n]; const iso_stream_t *v = &d->stream;
        if (!d->serial[0]) continue;
        printf("KMSISO DEVICE serial=%s address=%" PRIu32 " source=%" PRIu32 " count=%" PRIu32
               " crc=%08" PRIx32 " sequence-errors=%" PRIu32 " payload-errors=%" PRIu32
               " usb-errors=%" PRIu32 " stale=%" PRIu32 " releases=%" PRIu32
               " pending=%" PRIu32 " control-pending=%" PRIu32 " control-start=%" PRIu32
               " controls=%" PRIu32 " last-report=%" PRIu32
               " submissions=%" PRIu32 " callbacks=%" PRIu32 " transfer=%08" PRIx32 "\n",
               d->serial, d->address, v->source, v->count, v->crc, v->sequence_errors,
               v->payload_errors, d->usb_errors, v->stale, v->releases, d->pending,
               d->control_pending, d->control_start, d->controls, d->last_report,
               d->submissions, d->callbacks, d->transfer);
    }
    if (detail) {
        const bus_sample_t *current = &s->bus;
        printf("KMSISO BUS at=%" PRIu32 " frame=%08" PRIx32 " port=%08" PRIx32
               " core=%08" PRIx32 " mask=%08" PRIx32 " channels=%08" PRIx32
               " ahb=%08" PRIx32 " clock=%08" PRIx32 "\n", current->at, current->frame, current->port,
               current->core, current->mask, current->channels, current->ahb, current->clock);
        printf("KMSISO CORE reset=%08" PRIx32 " nptx=%08" PRIx32 " ptx=%08" PRIx32
               " host=%08" PRIx32 " interval=%08" PRIx32 " frame-list=%08" PRIx32
               " channel-mask=%08" PRIx32 "\n", s->reset, s->nptx, s->ptx, s->host,
               s->interval, s->frame_list, s->channel_mask);
        for (unsigned n = 0; n < s->samples_count; ++n) {
            const bus_sample_t *b = &s->samples[n];
            printf("KMSISO SAMPLE at=%" PRIu32 " frame=%08" PRIx32 " port=%08" PRIx32
                   " core=%08" PRIx32 " mask=%08" PRIx32 " channels=%08" PRIx32
                   " ahb=%08" PRIx32 " clock=%08" PRIx32 "\n", b->at, b->frame, b->port,
                   b->core, b->mask, b->channels, b->ahb, b->clock);
        }
        for (unsigned n = 0; n < 8; ++n) {
            const uint32_t *c = s->channels[n];
            printf("KMSISO CHANNEL n=%u char=%08" PRIx32 " irq=%08" PRIx32 " mask=%08" PRIx32
                   " size=%08" PRIx32 " dma=%08" PRIx32 " buffer=%08" PRIx32 "\n",
                   n, c[0], c[1], c[2], c[3], c[4], c[5]);
        }
    }
#ifdef ISO_DRIVER_TRACE
    if (detail && s->frozen) iso_driver_print();
#endif
    printf("KMSISO END id=%u\n", token);
}
static void console(const char *line)
{
    if (!strcmp(line, "iso info")) {
        printf("KMSISO INFO 1 %s %08" PRIx32 " %s idf=%s usb=1.6.0" ISO_TRACE_IDENTITY "\n", identity, boot,
               esp_app_get_description()->version, esp_app_get_description()->idf_ver); return;
    }
    command_t command = {0}; int used = 0; unsigned token = 0;
    if (sscanf(line, "iso arm %u %u %u %u %u%n", &command.run, &command.seed, &command.profile,
               &command.mode, &command.expected, &used) == 5 && !line[used]) command.kind = CMD_ARM;
    else if (sscanf(line, "iso watch %u%n", &command.watching, &used) == 1 && !line[used]) command.kind = CMD_WATCH;
    else if (sscanf(line, "iso workaround %u%n", &command.workaround, &used) == 1 && !line[used]) command.kind = CMD_WORKAROUND;
    else if (sscanf(line, "iso status %u%n", &token, &used) == 1 && token && !line[used]) command.kind = CMD_STATUS;
    else if (sscanf(line, "iso fault %u%n", &token, &used) == 1 && token && !line[used]) command.kind = CMD_FAULT;
    else { printf("KMSISO ERROR invalid command\n"); return; }
    if (xQueueSend(requests, &command, 0) != pdTRUE) { printf("KMSISO ERROR command busy\n"); return; }
    reply_t reply;
    // The worker does no UART output. A command is never abandoned with a stale
    // reply in flight; host software enforces its own bounded wait/cancellation.
    if (xQueueReceive(replies, &reply, portMAX_DELAY) != pdTRUE) return;
    if (reply.result) { printf("KMSISO ERROR state %u\n", reply.result); return; }
    if (command.kind == CMD_STATUS || command.kind == CMD_FAULT)
        print_snapshot(token, &reply.snapshot, command.kind == CMD_FAULT);
    else printf("KMSISO OK run=%" PRIu32 " watching=%" PRIu32 "\n", reply.snapshot.run, reply.snapshot.watching);
}
void app_main(void)
{
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA)); boot = esp_random();
    snprintf(identity, sizeof(identity), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("KMSISO BOOT %08" PRIx32 "\n", boot);
    requests = xQueueCreate(1, sizeof(command_t)); replies = xQueueCreate(1, sizeof(reply_t));
    configASSERT(requests && replies);
    configASSERT(xTaskCreatePinnedToCore(worker, "iso-usb", 8192, NULL, 5, NULL, 1) == pdPASS);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    char line[160]; unsigned length = 0; bool overflow = false;
    while (true) {
        char c;
        while (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == '\r' || c == '\n') {
                line[length] = 0;
                if (overflow) printf("KMSISO ERROR line too long\n");
                else if (length) console(line);
                length = 0; overflow = false;
            } else if (length < sizeof(line) - 1) line[length++] = c;
            else overflow = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
