// Production fault capture and read-only bus sampling with fake registers/console.
#define kms_usb_input_event kms_diagnostics_event
#define kms_usb_input_selected kms_link_selected
#define kms_usb_input_capture_event kms_diagnostics_capture_event
#include "usb_fault.c"
#include "usb_bus.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
usb_dev_t USB0;
static unsigned checks, retained, queue_calls;
static uint32_t now;
static uint32_t fields[32];
static bool queue_available = true;
static bool fault_memory;
static kms_usb_fault_snapshot_t fault_storage;
static kms_usb_fault_snapshot_t queued;
static kms_usb_health_trace_t trace;
static kms_usb_bus_history_t progress;
void kms_usb_driver_snapshot(kms_usb_driver_snapshot_t *out)
{ memset(out, 0, sizeof(*out)); out->root_interrupts = 123; }
void kms_usb_driver_activity(uint32_t *interrupts, uint32_t *last_interrupt)
{ *interrupts = now / 2; *last_interrupt = now - 3; }
static usb_dev_t before;
uint32_t kms_now(void) { return now; }
bool kms_time_elapsed(uint32_t n, uint32_t since, uint32_t duration) { return n - since >= duration; }
unsigned kms_link_selected(void) { return 3; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void *kms_memory_calloc(size_t count, size_t bytes)
{ return fault_memory && count == 1 && bytes == sizeof(fault_storage) ? &fault_storage : NULL; }
void kms_diagnostics_capture_event(kms_diag_record_t *out, enum kms_event event, unsigned slot, uint32_t detail)
{ *out = (kms_diag_record_t){.version = 1, .boot = 1234, .uptime_ms = now, .event = (uint16_t)event,
    .slot = (uint16_t)slot, .detail = detail}; }
void kms_diagnostics_event(enum kms_event event, unsigned slot, uint32_t detail)
{ (void)event; (void)slot; if (retained < 32) fields[retained++] = detail; }
bool kms_usb_console_fault_queue(const struct kms_usb_fault_snapshot *snapshot)
{ ++queue_calls; if (!queue_available) return false; queued = *snapshot; return true; }
void kms_usb_devices_fault_snapshot(kms_usb_inventory_t *out)
{
    memset(out, 0, sizeof(*out)); out->device_count = 1;
    out->devices[0] = (kms_usb_device_info_t){.address = 3, .vid = 0x046d, .pid = 0xc534, .parent = 1, .port = 4};
    out->stream.callback.max_us = 12; out->stream.poll_gap.max_us = 50000; out->stream.poll_gap.at = now - 500;
    out->stream.recent_count = 1; out->stream.recent[0] = (kms_usb_delay_t){.at = now - 500, .gap_us = 50000};
}
__declspec(dllexport) unsigned kms_usb_fault_test_count(void) { return checks; }
__declspec(dllexport) unsigned kms_usb_fault_snapshot_bytes(void) { return sizeof(queued); }
__declspec(dllexport) int kms_run_usb_fault_tests(void)
{
    static int owner;
    const uint8_t setup[8] = {0x80, 0, 0x34, 0x12, 0x78, 0x56, 2, 0};
    kms_usb_health_init(); CHECK(kms_usb_fault_latest() == NULL);
    kms_usb_fault_init(); kms_usb_fault_capture(NULL);
    CHECK(kms_usb_fault_latest() == NULL && !queue_calls);
    fault_memory = true; kms_usb_fault_init();
    CHECK(kms_usb_fault_latest() == NULL);
    now = 0xfffffff0u; USB0.hfnum = 65535; USB0.hprt = 0x12345678;
    USB0.gintsts = 0x11223344; USB0.gintmsk = 0x55667788; USB0.haint = 255;
    USB0.gahbcfg = 0x21; USB0.gusbcfg = 0x40001400; USB0.grstctl = 0xc0000000;
    USB0.gnptxsts = 0x00080020; USB0.hptxsts = 0x00100040; USB0.hcfg = 0x401;
    USB0.hfir = 48000; USB0.hflbaddr = 0x3fc82000; USB0.pcgctrl = 2; USB0.haintmsk = 0xff;
    for (unsigned n = 0; n < 128; ++n) USB0.host_chan_regs[n] = n + 200;
    kms_usb_bus_sample(); now += 20; USB0.hfnum = 0; kms_usb_bus_sample();
    CHECK(last_change == now); // Both the clock and the frame counter wrap.
    now += 100; kms_usb_bus_sample(); CHECK(now - last_change == 100);
    kms_usb_health_begin(&owner, KMS_USB_PROBE, 3, 0);
    kms_usb_health_request(&owner, setup, 10);
    now += 1000;
    kms_usb_overdue_t reason; CHECK(kms_usb_health_overdue(now, 1000, &reason));
    before = USB0; kms_usb_fault_capture(&reason);
    CHECK(memcmp(&before, &USB0, sizeof(USB0)) == 0); // Reads cannot clear interrupts or pop RX data.
    CHECK(queue_calls == 1 && queued.sequence == 1 && queued.context.boot == 1234 && queued.context.slot == 3);
    CHECK(queued.bus.at - queued.bus.last_frame_change == 1100 && queued.bus.hprt == 0x12345678);
    CHECK(queued.driver.root_interrupts == 123);
    CHECK(queued.devices.stream.callback.max_us == 12 && queued.devices.stream.poll_gap.max_us == 50000);
    CHECK(queued.devices.stream.recent_count == 1 && queued.devices.stream.recent[0].at == now - 500);
    CHECK(queued.bus.channels[7].characteristics == 256 && queued.bus.channels[7].interrupt == 258);
    CHECK(queued.bus.channels[7].mask == 259 && queued.bus.channels[7].size == 260);
    CHECK(queued.bus.channels[7].dma == 261 && queued.bus.channels[7].buffer == 263);
    CHECK(queued.bus.gahbcfg == 0x21 && queued.bus.gusbcfg == 0x40001400 && queued.bus.grstctl == 0xc0000000);
    CHECK(queued.bus.gnptxsts == 0x00080020 && queued.bus.hptxsts == 0x00100040 && queued.bus.hcfg == 0x401);
    CHECK(queued.bus.hfir == 48000 && queued.bus.hflbaddr == 0x3fc82000 && queued.bus.pcgcctl == 2 && queued.bus.haintmsk == 255);
    CHECK(queued.bus_history.count == 2 && queued.bus_history.samples[0].at == 0xfffffff0u);
    CHECK(queued.bus_history.samples[1].at == now && queued.bus_history.samples[1].last_interrupt == now - 3);
    CHECK(queued.bus_history.samples[1].channels[7].dma == 261);
    CHECK(queued.controls.pending_count == 1 && queued.controls.pending[0].expected == 10);
    CHECK(memcmp(queued.controls.pending[0].setup, setup, 8) == 0);
    CHECK(retained == 11);
    CHECK(fields[0] == (KMS_USB_FAULT_EVENT_TAG | KMS_USB_FAULT_OPERATION << 16 | KMS_USB_PROBE << 8 | 3));
    CHECK(fields[3] == (KMS_USB_FAULT_EVENT_TAG | KMS_USB_FAULT_PORT_LOW << 16 | 0x5678));
    CHECK(fields[4] == (KMS_USB_FAULT_EVENT_TAG | KMS_USB_FAULT_PORT_HIGH << 16 | 0x1234));
    CHECK(fields[6] == (KMS_USB_FAULT_EVENT_TAG | KMS_USB_FAULT_REQUEST << 16 | 0x8000));
    CHECK(fields[7] == (KMS_USB_FAULT_EVENT_TAG | KMS_USB_FAULT_VALUE << 16 | 0x1234));
    CHECK(fields[8] == (KMS_USB_FAULT_EVENT_TAG | KMS_USB_FAULT_INDEX << 16 | 0x5678));
    CHECK(fields[9] == (KMS_USB_FAULT_EVENT_TAG | KMS_USB_FAULT_VID << 16 | 0x046d));
    CHECK(fields[10] == (KMS_USB_FAULT_EVENT_TAG | KMS_USB_FAULT_PID << 16 | 0xc534));
    kms_usb_health_complete(&owner, 3, 0, 10); // Recovery cancellation happens after capture.
    USB0.hprt = 0; USB0.hfnum = 10; now += 20; kms_usb_bus_sample();
    CHECK(queued.bus.hprt == 0x12345678 && queued.controls.pending_count == 1);
    CHECK(kms_usb_fault_latest()->controls.recent_count == 0);
    kms_usb_health_snapshot(&trace);
    CHECK(trace.pending_count == 0 && trace.recent_count == 1 && trace.recent[0].status == 3);
    CHECK(trace.recent[0].started + 1000 == trace.recent[0].finished);
    // Preserve short completions and submission failures as distinct evidence.
    kms_usb_health_begin(&owner, KMS_USB_PROBE, 3, 0); kms_usb_health_request(&owner, setup, 10);
    kms_usb_health_complete(&owner, 0, 8, 10);
    kms_usb_health_rejected(KMS_USB_LED, 4, 1, setup, 0x103);
    kms_usb_health_snapshot(&trace);
    CHECK(trace.recent_count == 3 && trace.recent[1].status == 0 && trace.recent[1].actual == 8 && trace.recent[1].expected == 10);
    CHECK(trace.recent[2].rejected && trace.recent[2].status == 0x103 && trace.recent[2].address == 4);
    for (unsigned n = 0; n < 12; ++n) {
        kms_usb_health_begin(&owner, KMS_USB_PROBE, 3, 0); now += 2;
        kms_usb_health_complete(&owner, 1, (int)n, 10);
    }
    kms_usb_health_snapshot(&trace);
    CHECK(trace.recent_count == KMS_USB_RECENT_CONTROLS);
    CHECK(trace.recent[0].actual == 4 && trace.recent[7].actual == 11);
    queue_available = false; retained = 0; kms_usb_fault_capture(&reason);
    CHECK(kms_usb_fault_latest()->sequence == 2 && queued.sequence == 1); // Busy UART cannot erase the latest snapshot.
    CHECK(kms_usb_fault_latest()->controls.recent_count == 8);
    retained = 0; kms_usb_fault_capture(NULL);
    CHECK(kms_usb_fault_latest()->reason.operation == 255 && retained == 6);
    CHECK(memcmp(kms_diag_event_name(KMS_EVENT_INPUT_ERROR, fields[0]), "USB fault operation/address",
                 sizeof("USB fault operation/address")) == 0);
    CHECK(memcmp(kms_diag_event_name(KMS_EVENT_INPUT_ERROR, KMS_USB_FAULT_EVENT_TAG | 255u << 16), "input error",
                 sizeof("input error")) == 0);
    uint32_t frozen_dma = kms_usb_fault_latest()->bus_history.samples[0].channels[7].dma;
    for (unsigned n = 0; n < KMS_USB_BUS_SAMPLES + 2; ++n) {
        now += KMS_USB_BUS_SAMPLE_MS; ++USB0.hfnum;
        USB0.host_chan_regs[7 * 8 + 5] = 0x100 + n; kms_usb_bus_sample();
    }
    kms_usb_bus_history(&progress);
    CHECK(progress.count == KMS_USB_BUS_SAMPLES);
    CHECK(progress.samples[0].channels[7].dma == 0x102 && progress.samples[5].channels[7].dma == 0x107);
    CHECK(progress.samples[5].at - progress.samples[0].at == 5 * KMS_USB_BUS_SAMPLE_MS);
    CHECK(progress.samples[5].interrupts == now / 2 && progress.samples[5].last_interrupt == now - 3);
    CHECK(kms_usb_fault_latest()->bus_history.samples[0].channels[7].dma == frozen_dma);
    now += 20; kms_usb_bus_sample(); kms_usb_bus_history(&progress);
    CHECK(progress.samples[5].at == now - 20); // Frame sampling cannot fill the slower history ring.
    _Static_assert(sizeof(kms_usb_fault_snapshot_t) < 6144, "Bound all retained/console fault copies");
    return 0;
}
