// Exercise production USB lifecycle code against asynchronous host completions.
#define kms_usb_input_event kms_diagnostics_event
#define kms_usb_input_selected kms_link_selected
#include "usb_devices.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
typedef struct {
    usb_device_desc_t desc;
    uint8_t config[128], parent;
    bool present, opened, claimed[4];
    const uint8_t *reports[4];
    unsigned report_bytes[4];
} fake_device_t;
typedef struct { usb_transfer_t t; uint8_t bytes[KMS_DESCRIPTOR_BYTES + 8]; bool used, inflight, finish; } fake_transfer_t;
static fake_device_t fake_devices[128];
static fake_transfer_t fake_transfers[32];
static usb_host_client_config_t host_config;
static usb_host_client_event_msg_t host_events[16];
static unsigned event_count, checks, mistakes, claims, releases, frees, attaches, removals, received;
static uint32_t fake_now;
static unsigned halt_calls, flush_calls;
static unsigned extra_controls;
static bool clobber_on_submit, checked_rearm, reject_report;
static esp_err_t report_rejection;
static bool hold_cancellations;
static unsigned claim_attempts, claim_failures, fail_address, fail_interface, early_claims;
static unsigned parser_failures, control_alloc_failures, input_alloc_failures;
static bool in_completion;
static unsigned inline_reports;
static unsigned callback_side_effects, raw_resets, processing_cost_us;
static uint32_t clock_last_ms, last_captured_at;
static int64_t clock_us;
static uint8_t received_values[128];
static kms_usb_policy_t saved;
static struct { uint64_t alignment; uint8_t bytes[16384]; bool used; } allocations[24];
static const uint8_t keyboard_report[] = {5,1,9,6,0xa1,1,5,7,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,1,0x81,0,0xc0};
static const uint8_t mouse_report[] = {5,1,9,2,0xa1,1,9,0x30,9,0x31,0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0};

#ifdef KMS_USB_SHARED_HEAP
int kms_shared_sdk_claim(unsigned address, unsigned number);
void kms_shared_sdk_release(unsigned address, unsigned number);
#else
void *calloc(size_t count, size_t bytes)
{
    if (in_completion) ++callback_side_effects;
    if (parser_failures && count == 1 && bytes == sizeof(kms_hid_parser_t)) { --parser_failures; return NULL; }
    if (!count || bytes > sizeof(allocations[0].bytes) / count) { ++mistakes; return NULL; }
    for (unsigned n = 0; n < 24; ++n) if (!allocations[n].used) {
        allocations[n].used = true; memset(allocations[n].bytes, 0, count * bytes); return allocations[n].bytes;
    }
    ++mistakes; return NULL;
}
void free(void *p)
{
    if (in_completion) ++callback_side_effects;
    if (!p) return;
    for (unsigned n = 0; n < 24; ++n) if (p == allocations[n].bytes && allocations[n].used) {
        allocations[n].used = false; return;
    }
    ++mistakes;
}
void *realloc(void *p, size_t bytes)
{
    if (in_completion) ++callback_side_effects;
    for (unsigned n = 0; n < 24; ++n)
        if (p == allocations[n].bytes && allocations[n].used && bytes <= sizeof(allocations[n].bytes)) return p;
    ++mistakes; return NULL;
}
#endif
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; if (in_completion) ++callback_side_effects; }
void kms_fake_check(int result) { if (result != ESP_OK) ++mistakes; }
void kms_usb_memory_init(void) { }
uint32_t kms_usb_memory_sequence(void) { return 0; }
void kms_usb_memory_failed(unsigned address, unsigned number, uint32_t before) { (void)address; (void)number; (void)before; }
void kms_host_assert(int condition) { if (!condition) ++mistakes; }
uint32_t kms_now(void) { return fake_now; }
int64_t esp_timer_get_time(void)
{
    clock_us += (int64_t)(uint32_t)(fake_now - clock_last_ms) * 1000 + 1;
    clock_last_ms = fake_now; return clock_us;
}
unsigned kms_link_selected(void) { return 2; }
bool kms_time_elapsed(uint32_t now, uint32_t since, uint32_t duration) { return now - since >= duration; }
void kms_diagnostics_event(enum kms_event event, unsigned slot, uint32_t detail) { (void)event; (void)slot; (void)detail; }
void kms_usb_preferences_init(kms_usb_policy_t *p) { *p = saved; }
bool kms_usb_preferences_save(const kms_usb_policy_t *p) { saved = *p; return true; }
const char *esp_err_to_name(esp_err_t error_code) { (void)error_code; return "fake error"; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return pdPASS; }
esp_err_t usb_host_install(const usb_host_config_t *config) { (void)config; return ESP_OK; }
esp_err_t usb_host_client_register(const usb_host_client_config_t *c, usb_host_client_handle_t *out)
{ host_config = *c; *out = &host_config; return ESP_OK; }
esp_err_t usb_host_lib_handle_events(int timeout, uint32_t *flags) { (void)timeout; *flags = 0; return ESP_OK; }
esp_err_t usb_host_device_free_all(void) { return ESP_OK; }
esp_err_t usb_host_lib_info(usb_host_lib_info_t *out)
{
    memset(out, 0, sizeof(*out)); out->num_devices = extra_controls;
    for (unsigned n = 1; n < 128; ++n) if (fake_devices[n].present) ++out->num_devices;
    return ESP_OK;
}
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t handle, int timeout)
{
    (void)handle; if (timeout) ++mistakes;
    for (unsigned n = 0; n < event_count; ++n) host_config.async.client_event_callback(&host_events[n], NULL);
    event_count = 0;
    for (unsigned n = 0; n < 32; ++n) if (fake_transfers[n].finish) {
        if (hold_cancellations && fake_transfers[n].t.bEndpointAddress &&
            fake_transfers[n].t.status != USB_TRANSFER_STATUS_COMPLETED) continue;
        fake_transfers[n].finish = fake_transfers[n].inflight = false;
        in_completion = true;
        fake_transfers[n].t.callback(&fake_transfers[n].t);
        in_completion = false;
    }
    return ESP_OK;
}
esp_err_t usb_host_device_addr_list_fill(int capacity, uint8_t *addresses, int *count)
{
    *count = 0;
    for (unsigned n = 1; n < 128 && *count < capacity; ++n)
        if (fake_devices[n].present) addresses[(*count)++] = (uint8_t)n;
    return ESP_OK;
}
esp_err_t usb_host_device_open(usb_host_client_handle_t c, uint8_t addr, usb_device_handle_t *out)
{
    (void)c;
    if (!fake_devices[addr].present) return ESP_FAIL;
    if (fake_devices[addr].opened) ++mistakes;
    fake_devices[addr].opened = true; *out = &fake_devices[addr]; return ESP_OK;
}
esp_err_t usb_host_device_close(usb_host_client_handle_t c, usb_device_handle_t handle)
{
    (void)c; fake_device_t *d = handle;
    if (!d->opened) ++mistakes;
    for (unsigned n = 0; n < 4; ++n) if (d->claimed[n]) ++mistakes;
    for (unsigned n = 0; n < 32; ++n)
        if (fake_transfers[n].used && fake_transfers[n].t.device_handle == handle) ++mistakes;
    d->opened = false; return ESP_OK;
}
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t h, const usb_device_desc_t **out)
{ *out = &((fake_device_t *)h)->desc; return ESP_OK; }
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t h, const usb_config_desc_t **out)
{ *out = (const usb_config_desc_t *)((fake_device_t *)h)->config; return ESP_OK; }
esp_err_t usb_host_device_info(usb_device_handle_t handle, usb_device_info_t *out)
{
    fake_device_t *d = handle; memset(out, 0, sizeof(*out));
    out->dev_addr = (uint8_t)(d - fake_devices); out->parent.dev_hdl = d->parent ? &fake_devices[d->parent] : NULL;
    out->parent.port_num = out->dev_addr; return ESP_OK;
}
esp_err_t usb_host_transfer_alloc(size_t bytes, int packets, usb_transfer_t **out)
{
    (void)packets;
    bool input_buffer = false;
    for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n)
        if (interfaces[n].claimed && !interfaces[n].transfer && !interfaces[n].stopping && interfaces[n].packet_bytes == bytes)
            input_buffer = true;
    if (input_buffer && input_alloc_failures) { --input_alloc_failures; return ESP_ERR_NO_MEM; }
    if (!input_buffer && bytes > 8 && control_alloc_failures) { --control_alloc_failures; return ESP_ERR_NO_MEM; }
    for (unsigned n = 0; n < 32; ++n) if (!fake_transfers[n].used) {
        fake_transfer_t *f = &fake_transfers[n];
        if (bytes > sizeof(f->bytes)) { ++mistakes; return ESP_FAIL; }
        memset(f, 0, sizeof(*f)); f->used = true; f->t.data_buffer = f->bytes;
        f->t.data_buffer_size = bytes; *out = &f->t; return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}
esp_err_t usb_host_transfer_free(usb_transfer_t *t)
{
    for (unsigned n = 0; n < 32; ++n) if (&fake_transfers[n].t == t) {
        fake_transfer_t *f = &fake_transfers[n];
        if (!f->used || f->inflight || f->finish) { ++mistakes; return ESP_FAIL; }
        f->used = false; ++frees; return ESP_OK;
    }
    ++mistakes; return ESP_FAIL;
}
esp_err_t usb_host_transfer_submit(usb_transfer_t *t)
{
    for (unsigned n = 0; n < 32; ++n) if (&fake_transfers[n].t == t && fake_transfers[n].used) {
        if (fake_transfers[n].inflight || fake_transfers[n].finish) ++mistakes;
        if (reject_report && t->bEndpointAddress) return report_rejection;
        fake_transfers[n].inflight = true;
        if (clobber_on_submit && t->bEndpointAddress) memset(t->data_buffer, 0xee, t->data_buffer_size);
        return ESP_OK;
    }
    ++mistakes; return ESP_FAIL;
}
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t c, usb_transfer_t *t)
{ (void)c; return usb_host_transfer_submit(t); }
esp_err_t usb_host_interface_claim(usb_host_client_handle_t c, usb_device_handle_t h, uint8_t number, uint8_t alt)
{
    (void)c; (void)alt; fake_device_t *d = h;
    if (number >= 4 || d->claimed[number]) { ++mistakes; return ESP_FAIL; }
    ++claim_attempts;
    for (unsigned address = 1; address < 128; ++address)
        if (!fake_devices[address].present)
            for (unsigned n = 0; n < 4; ++n) if (fake_devices[address].claimed[n]) ++early_claims;
    if (claim_failures && (!fail_address || d == &fake_devices[fail_address]) && number == fail_interface) {
        --claim_failures; return ESP_ERR_NO_MEM;
    }
    if (channels_used() + 1 > 8) return ESP_ERR_NOT_SUPPORTED;
#ifdef KMS_USB_SHARED_HEAP
    int result = kms_shared_sdk_claim((unsigned)(d - fake_devices), number);
    if (result != ESP_OK) return result;
#endif
    d->claimed[number] = true; ++claims; return ESP_OK;
}
esp_err_t usb_host_interface_release(usb_host_client_handle_t c, usb_device_handle_t h, uint8_t number)
{
    if (in_completion) ++callback_side_effects;
    (void)c; fake_device_t *d = h;
    if (!d->claimed[number]) { ++mistakes; return ESP_FAIL; }
    for (unsigned n = 0; n < 32; ++n) if (fake_transfers[n].used && fake_transfers[n].t.device_handle == h &&
        fake_transfers[n].t.bEndpointAddress == 0x81 + number && (fake_transfers[n].inflight || fake_transfers[n].finish)) ++mistakes;
#ifdef KMS_USB_SHARED_HEAP
    kms_shared_sdk_release((unsigned)(d - fake_devices), number);
#endif
    d->claimed[number] = false; ++releases; return ESP_OK;
}
esp_err_t usb_host_endpoint_halt(usb_device_handle_t h, uint8_t ep)
{ (void)h; (void)ep; if (in_completion) ++callback_side_effects; ++halt_calls; return ESP_OK; }
esp_err_t usb_host_endpoint_flush(usb_device_handle_t h, uint8_t ep)
{
    if (in_completion) ++callback_side_effects;
    ++flush_calls;
    for (unsigned n = 0; n < 32; ++n) if (fake_transfers[n].used && fake_transfers[n].inflight &&
        fake_transfers[n].t.device_handle == h && fake_transfers[n].t.bEndpointAddress == ep) {
        fake_transfers[n].t.status = USB_TRANSFER_STATUS_CANCELED; fake_transfers[n].finish = true;
    }
    return ESP_OK;
}
static void on_attach(kms_usb_interface_t *i, const kms_hid_parser_t *p) { (void)i; if (!p) ++mistakes; ++attaches; }
static bool on_report(kms_usb_interface_t *i, const kms_hid_parser_t *p, const uint8_t *data, size_t bytes, uint32_t captured_at)
{
    last_captured_at = captured_at; clock_us += processing_cost_us;
    kms_sample_t sample; ++received;
    if (received <= sizeof(received_values) && bytes) received_values[received - 1] = data[0];
    if (in_completion) ++inline_reports;
    for (unsigned n = 0; n < 32; ++n) if (&fake_transfers[n].t == i->transfer) {
        checked_rearm = fake_transfers[n].inflight && data != i->transfer->data_buffer && data[0] == 0x59;
    }
    return kms_hid_decode(p, data, bytes, &sample);
}
static void on_remove(kms_usb_interface_t *i) { (void)i; ++removals; }
static void on_overflow(void) { if (in_completion) ++callback_side_effects; ++raw_resets; }
static void reset(bool automatic)
{
    kms_usb_health_init();
    memset(devices, 0, sizeof(devices)); memset(interfaces, 0, sizeof(interfaces)); memset(pending, 0, sizeof(pending));
    hub_count = 0; memset(hubs, 0, sizeof(hubs));
    memset(fake_devices, 0, sizeof(fake_devices)); memset(fake_transfers, 0, sizeof(fake_transfers));
    memset(allocations, 0, sizeof(allocations));
    event_count = mistakes = claims = releases = frees = attaches = removals = received = extra_controls = 0; fake_now = 0;
    halt_calls = flush_calls = 0;
    clobber_on_submit = checked_rearm = reject_report = false;
    report_rejection = ESP_FAIL;
    hold_cancellations = false;
    claim_attempts = claim_failures = fail_address = fail_interface = early_claims = 0;
    parser_failures = control_alloc_failures = input_alloc_failures = 0;
    in_completion = false; inline_reports = 0;
    callback_side_effects = raw_resets = processing_cost_us = 0;
    clock_us = 0; clock_last_ms = last_captured_at = 0; memset(received_values, 0, sizeof(received_values));
    kms_usb_policy_reset(&saved, automatic);
    kms_usb_input_callbacks_t callbacks = {.attached = on_attach, .report = on_report, .removed = on_remove, .overflow = on_overflow};
    kms_usb_devices_init(&callbacks);
}
static void announce(unsigned address, unsigned parent, unsigned device_class, unsigned count)
{
    fake_device_t *d = &fake_devices[address]; memset(d, 0, sizeof(*d));
    d->present = true; d->parent = parent; d->desc.idVendor = 0x1241; d->desc.idProduct = 0x1203;
    usb_config_desc_t cfg = {.bLength = 9, .bDescriptorType = 2, .bNumInterfaces = (uint8_t)count, .bConfigurationValue = 1};
    unsigned at = sizeof(cfg); memcpy(d->config, &cfg, sizeof(cfg));
    for (unsigned n = 0; n < count; ++n) {
        usb_intf_desc_t iface = {.bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = (uint8_t)n,
            .bNumEndpoints = 1, .bInterfaceClass = (uint8_t)device_class, .bInterfaceSubClass = device_class == 3 ? 1 : 0};
        memcpy(d->config + at, &iface, sizeof(iface)); at += sizeof(iface);
        if (device_class == 3) {
            d->reports[n] = n ? mouse_report : keyboard_report;
            d->report_bytes[n] = n ? sizeof(mouse_report) : sizeof(keyboard_report);
            uint8_t hid[] = {9, 0x21, 0x11, 1, 0, 1, 0x22, (uint8_t)d->report_bytes[n], 0};
            memcpy(d->config + at, hid, sizeof(hid)); at += sizeof(hid);
        }
        usb_ep_desc_t ep = {.bLength = 7, .bDescriptorType = 5, .bEndpointAddress = (uint8_t)(0x81 + n),
            .bmAttributes = 3, .wMaxPacketSize = 8, .bInterval = 10};
        memcpy(d->config + at, &ep, sizeof(ep)); at += sizeof(ep);
    }
    ((usb_config_desc_t *)d->config)->wTotalLength = (uint16_t)at;
    if (device_class != 9) host_events[event_count++] = (usb_host_client_event_msg_t){.event = USB_HOST_CLIENT_EVENT_NEW_DEV, .new_dev = {.address = (uint8_t)address}};
}
static void finish_controls(int status, int short_by)
{
    for (unsigned n = 0; n < 32; ++n) {
        fake_transfer_t *f = &fake_transfers[n];
        if (!f->used || !f->inflight || f->finish || f->t.bEndpointAddress) continue;
        const usb_setup_packet_t *setup = (const usb_setup_packet_t *)f->t.data_buffer;
        fake_device_t *d = f->t.device_handle;
        if (setup->bRequest == 6) memcpy(f->t.data_buffer + 8, d->reports[setup->wIndex], d->report_bytes[setup->wIndex]);
        f->t.status = status; f->t.actual_num_bytes = f->t.num_bytes - short_by; f->finish = true;
    }
}
static void pump(unsigned ticks)
{
    while (ticks--) { finish_controls(USB_TRANSFER_STATUS_COMPLETED, 0); ++fake_now; kms_usb_devices_tick(); }
}
static void unplug(unsigned address)
{
    fake_devices[address].present = false;
    host_events[event_count++] = (usb_host_client_event_msg_t){.event = USB_HOST_CLIENT_EVENT_DEV_GONE,
        .dev_gone = {.dev_hdl = &fake_devices[address]}};
    for (unsigned n = 0; n < 32; ++n) if (fake_transfers[n].used && fake_transfers[n].inflight &&
        fake_transfers[n].t.device_handle == &fake_devices[address]) {
        fake_transfers[n].t.status = USB_TRANSFER_STATUS_NO_DEVICE; fake_transfers[n].finish = true;
    }
}
static kms_usb_interface_info_t *row(kms_usb_inventory_t *s, unsigned address, unsigned number)
{
    for (unsigned n = 0; n < s->interface_count; ++n)
        if (s->interfaces[n].address == address && s->interfaces[n].number == number) return &s->interfaces[n];
    return NULL;
}
static bool dispatch_report(unsigned address, unsigned number, uint8_t value, int bytes)
{
    for (unsigned n = 0; n < 32; ++n) {
        fake_transfer_t *f = &fake_transfers[n];
        if (!f->used || !f->inflight || f->t.device_handle != &fake_devices[address] ||
            f->t.bEndpointAddress != 0x81 + number) continue;
        f->t.data_buffer[0] = value; f->t.actual_num_bytes = bytes;
        f->t.status = USB_TRANSFER_STATUS_COMPLETED; f->finish = true;
        (void)usb_host_client_handle_events(client, 0); return true;
    }
    return false;
}
__declspec(dllexport) unsigned kms_usb_host_test_count(void) { return checks; }
__declspec(dllexport) int kms_run_usb_host_tests(void)
{
    static kms_usb_inventory_t s;
    reset(false); announce(1, 0, 9, 1); announce(2, 1, 3, 2); kms_usb_devices_tick();
    CHECK(claims == 0 && attaches == 0); // Discovery uses EP0 without claiming input endpoints.
    pump(8); kms_usb_devices_snapshot(&s);
    CHECK(s.controls == 2 && s.hubs == 1 && s.inputs == 0);
    CHECK(s.device_count == 2 && !fake_devices[1].opened); // Hubs are discovered despite suppressed events and never pinned.
    CHECK(row(&s, 2, 0)->kinds == KMS_USB_KEYBOARD && row(&s, 2, 1)->kinds == KMS_USB_MOUSE);
    CHECK(kms_usb_devices_select(2, 0, true)); pump(6);
    CHECK(attaches == 2 && claims == 2);
    for (unsigned n = 0; n < 32; ++n) if (fake_transfers[n].used && fake_transfers[n].inflight && fake_transfers[n].t.bEndpointAddress == 0x81) {
        fake_transfers[n].t.data_buffer[0] = 0x59; fake_transfers[n].t.actual_num_bytes = 1;
        fake_transfers[n].t.status = USB_TRANSFER_STATUS_COMPLETED; fake_transfers[n].finish = true;
    }
    clobber_on_submit = true;
    kms_usb_devices_tick(); kms_usb_devices_snapshot(&s);
    CHECK(received == 1 && row(&s, 2, 0)->reports == 1 && row(&s, 2, 0)->decoded == 1);
    CHECK(checked_rearm); // DMA is already polling and may overwrite its buffer before decoding the copy.
    CHECK(inline_reports == 0); // Decoding must run after the SDK completion callback returns.
    clobber_on_submit = false;
    unsigned before_free = frees;
    CHECK(kms_usb_devices_select(2, 0, false));
    CHECK(removals == 2 && frees == before_free && releases == 0); // Cancellation must finish before free/release.
    pump(4); CHECK(releases == 2 && frees > before_free && mistakes == 0);
    CHECK(kms_usb_devices_select(2, 0, true)); pump(6); CHECK(attaches == 4);
    unplug(2); pump(3); CHECK(!fake_devices[2].opened && removals == 4 && mistakes == 0);
    announce(4, 1, 3, 2); pump(8); kms_usb_devices_snapshot(&s);
    CHECK(row(&s, 4, 0)->state == KMS_USB_ACTIVE && row(&s, 4, 1)->state == KMS_USB_ACTIVE); // Saved choice survives a new USB address.
    CHECK(mistakes == 0);

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 9, 1); announce(3, 2, 3, 2); pump(8); announce(4, 2, 3, 2);
    fake_devices[4].desc.idVendor = 0x046d; fake_devices[4].desc.idProduct = 0xc534;
    pump(10); kms_usb_devices_snapshot(&s);
    CHECK(s.controls == 4 && s.hubs == 2 && s.inputs == 2);
    unsigned active_devices = 0;
    for (unsigned address = 3; address <= 4; ++address)
        if (row(&s, address, 0)->state == KMS_USB_ACTIVE || row(&s, address, 1)->state == KMS_USB_ACTIVE) ++active_devices;
    CHECK(active_devices == 1); // Two complete composite bundles cannot fit in the remaining two channels.
    CHECK(kms_usb_devices_select(3, 1, false));
    CHECK(kms_usb_devices_select(4, 0, true)); pump(12); kms_usb_devices_snapshot(&s);
    CHECK(s.controls + s.hubs + s.inputs == 8);
    CHECK(row(&s, 3, 0)->state == KMS_USB_OFF && row(&s, 4, 1)->state == KMS_USB_ACTIVE);
    CHECK(row(&s, 3, 1)->state == KMS_USB_OFF && row(&s, 4, 0)->state == KMS_USB_ACTIVE && mistakes == 0);
    CHECK(kms_usb_devices_automatic(false)); pump(5); kms_usb_devices_snapshot(&s);
    CHECK(s.inputs == 2 && mistakes == 0); // Entering manual mode preserves working inputs.
    CHECK(kms_usb_devices_select(3, 0, false) && kms_usb_devices_select(4, 1, false));
    pump(5); kms_usb_devices_snapshot(&s);
    CHECK(s.inputs == 0 && removals == attaches && mistakes == 0);

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); kms_usb_devices_tick();
    before_free = frees; fake_now = 3000; kms_usb_devices_tick();
    CHECK(frees == before_free && fake_devices[2].opened && claims == 0); // A pending control transfer owns its buffer after the watchdog.
    unplug(2); kms_usb_devices_tick(); pump(2);
    CHECK(!fake_devices[2].opened && frees > before_free && mistakes == 0);
    announce(2, 1, 3, 1); kms_usb_devices_tick(); finish_controls(USB_TRANSFER_STATUS_ERROR, 0); kms_usb_devices_tick();
    kms_usb_devices_snapshot(&s); CHECK(row(&s, 2, 0)->state == KMS_USB_ERROR && claims == 0);
    kms_usb_devices_rescan(); kms_usb_devices_tick(); finish_controls(USB_TRANSFER_STATUS_COMPLETED, 1); kms_usb_devices_tick();
    kms_usb_devices_snapshot(&s); CHECK(row(&s, 2, 0)->state == KMS_USB_ERROR && claims == 0);
    kms_usb_devices_rescan(); pump(8); CHECK(attaches == 1 && mistakes == 0);
    for (unsigned n = 0; n < 32; ++n) if (fake_transfers[n].used && fake_transfers[n].inflight && fake_transfers[n].t.bEndpointAddress) {
        fake_transfers[n].t.status = USB_TRANSFER_STATUS_ERROR; fake_transfers[n].finish = true;
    }
    pump(4); kms_usb_devices_snapshot(&s);
    CHECK(removals == 1 && row(&s, 2, 0)->state == KMS_USB_ERROR && mistakes == 0);
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2); announce(3, 1, 3, 2);
    fake_devices[3].desc.idProduct = 0x4321;
    kms_usb_devices_tick(); pump(1);
    CHECK(claims == 0); // Wait for all HID descriptors, never start an incomplete physical input.
    pump(10); kms_usb_devices_snapshot(&s);
    CHECK(s.controls == 3 && s.hubs == 1 && s.inputs == 4 && attaches == 4 && removals == 0 && mistakes == 0);
    CHECK(kms_usb_devices_select(2, 1, false)); pump(5); kms_usb_devices_snapshot(&s);
    CHECK(s.inputs == 2 && row(&s, 2, 0)->state == KMS_USB_OFF && row(&s, 2, 1)->state == KMS_USB_OFF);
    CHECK(kms_usb_devices_select(2, 0, true)); pump(10); kms_usb_devices_snapshot(&s);
    CHECK(s.inputs == 4 && row(&s, 2, 0)->state == KMS_USB_ACTIVE && row(&s, 2, 1)->state == KMS_USB_ACTIVE && mistakes == 0);

    reset(true); extra_controls = 4; announce(1, 0, 9, 1); announce(2, 1, 3, 2); pump(12);
    kms_usb_devices_snapshot(&s); CHECK(s.inputs == 0);
    extra_controls = 0; pump(12); kms_usb_devices_snapshot(&s);
    CHECK(s.inputs == 2 && mistakes == 0); // Daemon-freed control channels trigger a new plan without another device event.

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); announce(3, 1, 3, 1); announce(4, 1, 3, 1);
    fake_devices[3].desc.idProduct = 0x4321; fake_devices[4].desc.idProduct = 0x5678;
    pump(12); kms_usb_devices_snapshot(&s);
    CHECK(s.inputs == 2 && s.controls + s.hubs + s.inputs < 8 && mistakes == 0);
    unplug(2); pump(12); kms_usb_devices_snapshot(&s);
    CHECK(s.inputs == 2 && row(&s, 4, 0)->state == KMS_USB_ACTIVE && mistakes == 0);
    unplug(3); unplug(4); pump(4);
    fake_devices[1].present = false; pump(4); kms_usb_devices_snapshot(&s);
    CHECK(s.hubs == 0 && s.device_count == 0 && mistakes == 0); // No hub DEV_GONE event is required for cleanup.

    // Standard device probes use the existing EP0 and leave complete HID bundles
    // active. No interrupt report is needed to prove the transport is alive.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2); pump(12);
    CHECK(kms_usb_devices_active() == 2 && kms_usb_devices_healthy() == 0);
    fake_now += KMS_USB_PROBE_MS; pump(2);
    CHECK(kms_usb_devices_healthy() == 2 && attaches == 2 && removals == 0 && mistakes == 0);
    kms_usb_overdue_t overdue;
    CHECK(!kms_usb_health_overdue(fake_now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue));
    fake_now += KMS_USB_PROBE_MS; kms_usb_devices_tick();
    CHECK(kms_usb_health_pending() == 1);
    fake_now += KMS_USB_CONTROL_TIMEOUT_MS;
    CHECK(kms_usb_health_overdue(fake_now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue));
    CHECK(overdue.operation == KMS_USB_PROBE && overdue.address == 2);
    before_free = frees; unsigned before_halt = halt_calls, before_flush = flush_calls;
    kms_usb_devices_quiesce(true); kms_usb_devices_tick();
    CHECK(halt_calls == before_halt && flush_calls == before_flush); // Quiesce must not enter a blocking SDK halt on the stalled bus.
    CHECK(removals == 2 && !kms_usb_devices_drained() && frees >= before_free);
    CHECK(kms_usb_health_pending() == 1); // The overdue control remains owned until its actual callback.
    unplug(2); kms_usb_devices_tick(); pump(2);
    CHECK(kms_usb_devices_drained() && kms_usb_health_pending() == 0 && mistakes == 0);
    fake_devices[1].present = false; kms_usb_devices_quiesce(false);
    announce(3, 0, 9, 1); announce(4, 3, 3, 2); pump(12);
    CHECK(kms_usb_devices_active() == 2 && mistakes == 0); // Normal discovery resumes with fresh addresses.

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    for (unsigned n = 0; n < KMS_USB_PROBE_FAILURES; ++n) {
        fake_now += KMS_USB_PROBE_MS; kms_usb_devices_tick();
        finish_controls(USB_TRANSFER_STATUS_ERROR, 0); kms_usb_devices_tick();
    }
    CHECK(kms_usb_health_overdue(fake_now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue));
    CHECK(overdue.operation == KMS_USB_PROBE && kms_usb_devices_active() == 1 && mistakes == 0);

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    reject_report = true;
    for (unsigned n = 0; n < 32; ++n) if (fake_transfers[n].used && fake_transfers[n].inflight && fake_transfers[n].t.bEndpointAddress) {
        fake_transfers[n].t.data_buffer[0] = 4; fake_transfers[n].t.actual_num_bytes = 1;
        fake_transfers[n].t.status = USB_TRANSFER_STATUS_COMPLETED; fake_transfers[n].finish = true;
    }
    kms_usb_devices_tick(); pump(3); kms_usb_devices_snapshot(&s);
    CHECK(row(&s, 2, 0)->state == KMS_USB_ERROR && removals == 1 && received == 0);
    CHECK(mistakes == 0); // Rearm failure retires the source before exposing its last buffer.
    fake_now += 2000; pump(20); kms_usb_devices_snapshot(&s);
    CHECK(row(&s, 2, 0)->state == KMS_USB_ERROR && claims == 1); // Hard I/O errors are not allocation retries.

    // An allocator failure during a full eight-channel reconnect must not
    // permanently exclude an otherwise valid composite receiver.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2); announce(3, 1, 3, 2);
    fake_devices[3].desc.idProduct = 0x4321;
    fail_address = 3; fail_interface = 1; claim_failures = 1;
    pump(20); kms_usb_devices_snapshot(&s);
    CHECK(row(&s, 2, 0)->state == KMS_USB_ACTIVE && row(&s, 2, 1)->state == KMS_USB_ACTIVE);
    unsigned before_attempts = claim_attempts;
    pump(100); CHECK(claim_attempts == before_attempts); // No millisecond allocation retry loop.
    fake_now += 1000; pump(20); kms_usb_devices_snapshot(&s);
    CHECK(s.inputs == 4 && kms_usb_devices_active() == 4);
    CHECK(row(&s, 3, 0)->state == KMS_USB_ACTIVE && row(&s, 3, 1)->state == KMS_USB_ACTIVE);
    CHECK(mistakes == 0);

    // New addresses can enumerate while old cancellation callbacks still own
    // their endpoint channels, parsers and transfers. Drain those first.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2); pump(12);
    hold_cancellations = true; unplug(2); announce(3, 1, 3, 2); pump(20);
    CHECK(early_claims == 0 && fake_devices[2].opened && releases == 0);
    hold_cancellations = false; pump(20); kms_usb_devices_snapshot(&s);
    CHECK(!fake_devices[2].opened && kms_usb_devices_active() == 2 && mistakes == 0);

    // Repeated pressure stays bounded and unplugging the waiting receiver
    // cancels its retry, without resetting or stopping the other receiver.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2); announce(3, 1, 3, 2);
    fake_devices[3].desc.idProduct = 0x4321;
    fail_address = 3; fail_interface = 1; claim_failures = 100;
    pump(20); before_attempts = claim_attempts;
    pump(100); CHECK(claim_attempts == before_attempts);
    fake_now += 1000; pump(20);
    CHECK(claim_attempts > before_attempts && claim_failures == 98);
    CHECK(kms_usb_devices_active() == 2 && mistakes == 0);
    unplug(3); pump(20); before_attempts = claim_attempts;
    fake_now += 2000; pump(20);
    CHECK(claim_attempts == before_attempts && kms_usb_devices_active() == 2 && mistakes == 0);

    // Allocation failures can also happen before a claim, or after a claim
    // succeeds. Each path must retry without leaking the partial ownership.
    for (unsigned stage = 0; stage < 3; ++stage) {
        reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2);
        if (stage == 0) parser_failures = 1;
        else if (stage == 1) control_alloc_failures = 1;
        else input_alloc_failures = 1;
        pump(20);
        CHECK(kms_usb_devices_active() == 0 && mistakes == 0);
        fake_now += 1000; pump(20); kms_usb_devices_snapshot(&s);
        CHECK(kms_usb_devices_active() == 2 && s.inputs == 2 && mistakes == 0);
        unplug(2); pump(10);
        CHECK(claims == releases && !fake_devices[2].opened && mistakes == 0);
    }

    reset(true); fake_now = UINT32_MAX - 500;
    announce(1, 0, 9, 1); announce(2, 1, 3, 2);
    fail_address = 2; fail_interface = 1; claim_failures = 1; pump(20);
    before_attempts = claim_attempts; fake_now += 600; pump(20);
    CHECK(claim_attempts == before_attempts); // Backoff remains in force across clock wrap.
    fake_now += 500; pump(20);
    CHECK(kms_usb_devices_active() == 2 && mistakes == 0);

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2);
    fail_address = 2; fail_interface = 1; claim_failures = 100; pump(20);
    CHECK(kms_usb_devices_select(2, 0, false)); before_attempts = claim_attempts;
    fake_now += 1000; pump(20); kms_usb_devices_snapshot(&s);
    CHECK(claim_attempts == before_attempts && s.inputs == 0 && mistakes == 0);
    CHECK(row(&s, 2, 0)->state == KMS_USB_OFF && row(&s, 2, 1)->state == KMS_USB_OFF);

    // Repeated removal/replacement at full capacity must release every old
    // claim exactly once and resume complete keyboard/pointer bundles.
    reset(true); announce(1, 0, 9, 1);
    unsigned first = 2, second = 3;
    announce(first, 1, 3, 2); announce(second, 1, 3, 2);
    fake_devices[second].desc.idProduct = 0x4321; pump(20);
    for (unsigned cycle = 0; cycle < 12; ++cycle) {
        hold_cancellations = true; unplug(first); unplug(second);
        first += 2; second += 2;
        announce(first, 1, 3, 2); announce(second, 1, 3, 2);
        fake_devices[second].desc.idProduct = 0x4321; pump(10);
        CHECK(early_claims == 0 && mistakes == 0);
        hold_cancellations = false; pump(25); kms_usb_devices_snapshot(&s);
        CHECK(kms_usb_devices_active() == 4 && s.controls + s.hubs + s.inputs == 8);
        CHECK(claims == releases + 4 && mistakes == 0);
    }

    // A slow consumer cannot extend a completion callback or delay dispatching
    // another endpoint. Copies survive immediate DMA-buffer reuse, in order.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    clobber_on_submit = true; processing_cost_us = 5000;
    for (unsigned n = 0; n < 8; ++n) CHECK(dispatch_report(2, 0, (uint8_t)(4 + n), 1));
    CHECK(received == 0 && inline_reports == 0 && callback_side_effects == 0);
    kms_usb_devices_snapshot(&s); CHECK(s.stream.pending == 8 && s.stream.peak == 8);
    kms_usb_devices_tick(); kms_usb_devices_snapshot(&s);
    CHECK(received == 8 && s.stream.pending == 0 && s.stream.decode.max_us >= 5000);
    CHECK(s.stream.callback.max_us < 100 && s.stream.dispatch.max_us < 100);
    for (unsigned n = 0; n < 8; ++n) CHECK(received_values[n] == 4 + n);
    kms_usb_devices_tick(); kms_usb_devices_snapshot(&s);
    CHECK(s.stream.poll_gap.max_us >= 40000 && s.stream.recent_count == 1);
    CHECK(s.stream.recent[0].gap_us >= 40000 && callback_side_effects == 0 && mistakes == 0);

    // Filling the raw ring must keep reads armed. Overflow signals the same
    // held-input protection as application-queue overflow, outside callbacks.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    for (unsigned n = 0; n < KMS_USB_RAW_QUEUE_DEPTH + 3; ++n) CHECK(dispatch_report(2, 0, 4, 1));
    CHECK(received == 0 && raw_resets == 0 && callback_side_effects == 0);
    kms_usb_devices_snapshot(&s); CHECK(s.stream.pending == KMS_USB_RAW_QUEUE_DEPTH && s.stream.overflows == 1);
    kms_usb_devices_tick(); kms_usb_devices_snapshot(&s);
    CHECK(raw_resets == 1 && received == 0 && s.stream.pending == 0);
    CHECK(s.stream.dropped == KMS_USB_RAW_QUEUE_DEPTH + 3 && row(&s, 2, 0)->transfer_pending);
    CHECK(dispatch_report(2, 0, 0, 1)); kms_usb_devices_tick();
    CHECK(received == 1 && received_values[0] == 0 && mistakes == 0);

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    uint32_t captured_at = fake_now; CHECK(dispatch_report(2, 0, 5, 1)); fake_now += 150;
    kms_usb_devices_tick(); CHECK(received == 1 && last_captured_at == captured_at);
    CHECK(dispatch_report(2, 0, 6, 1)); fake_now += KMS_USB_MAX_AGE_MS;
    kms_usb_devices_tick(); kms_usb_devices_snapshot(&s);
    CHECK(received == 1 && raw_resets == 1 && s.stream.expired == 1 && s.stream.pending == 0);

    // Removing/reusing a source cannot decode through its freed parser.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    CHECK(dispatch_report(2, 0, 7, 1)); unplug(2); announce(3, 1, 3, 1); pump(12);
    kms_usb_devices_snapshot(&s);
    CHECK(received == 0 && s.stream.stale == 1 && row(&s, 3, 0)->state == KMS_USB_ACTIVE && mistakes == 0);
    CHECK(dispatch_report(3, 0, 8, 1));
    ++raw_reports[raw_head].generation; // An obsolete activation token must also be rejected.
    kms_usb_devices_tick(); kms_usb_devices_snapshot(&s);
    CHECK(received == 0 && s.stream.stale == 2 && mistakes == 0);
    CHECK(dispatch_report(3, 0, 9, 1)); kms_usb_devices_quiesce(true); kms_usb_devices_tick();
    CHECK(received == 0 && raw_count == 0 && callback_side_effects == 0 && mistakes == 0);

    // Failure handling itself must not log, halt, flush or free inside the
    // completion callback, and its last report must not reach the consumer.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    reject_report = true; CHECK(dispatch_report(2, 0, 4, 1));
    CHECK(callback_side_effects == 0 && removals == 0 && received == 0);
    pump(4); kms_usb_devices_snapshot(&s);
    CHECK(row(&s, 2, 0)->state == KMS_USB_ERROR && removals == 1 && received == 0);
    CHECK(callback_side_effects == 0 && mistakes == 0);

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    CHECK(dispatch_report(2, 0, 4, 0)); CHECK(dispatch_report(2, 0, 4, 9)); CHECK(dispatch_report(2, 0, 4, -1));
    kms_usb_devices_tick(); kms_usb_devices_snapshot(&s);
    CHECK(received == 0 && s.stream.pending == 0 && row(&s, 2, 0)->transfer_pending && mistakes == 0);

    // Recent delay history is bounded, ordered, and an immutable snapshot.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    for (unsigned n = 0; n < KMS_USB_STREAM_DELAYS + 3; ++n) { fake_now += 6; kms_usb_devices_tick(); }
    kms_usb_devices_snapshot(&s);
    CHECK(s.stream.recent_count == KMS_USB_STREAM_DELAYS);
    for (unsigned n = 1; n < s.stream.recent_count; ++n) CHECK(s.stream.recent[n].at > s.stream.recent[n - 1].at);
    captured_at = s.stream.recent[0].at; fake_now += 50; kms_usb_devices_tick();
    CHECK(s.stream.recent[0].at == captured_at && stream.poll_gap.max_us >= 50000 && mistakes == 0);

    // Four 1 kHz endpoints, serviced in two-frame batches, retain every report
    // at the normal eight-channel limit without allocating in a completion.
    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2); announce(3, 1, 3, 2);
    fake_devices[3].desc.idProduct = 0x4321; pump(20); clobber_on_submit = true;
    for (unsigned batch = 0; batch < 250; ++batch) {
        for (unsigned frame = 0; frame < 2; ++frame) {
            ++fake_now;
            for (unsigned address = 2; address <= 3; ++address)
                for (unsigned number = 0; number < 2; ++number)
                    if (!dispatch_report(address, number, 0x59, number ? 2 : 1)) return __LINE__;
        }
        kms_usb_devices_tick();
    }
    kms_usb_devices_snapshot(&s);
    CHECK(received == 2000 && s.stream.overflows == 0 && s.stream.peak == 8);
    CHECK(kms_usb_devices_active() == 4 && inline_reports == 0 && callback_side_effects == 0 && mistakes == 0);

    reset(true); announce(1, 0, 9, 1); announce(2, 1, 3, 2); pump(20);
    reject_report = true; report_rejection = ESP_ERR_NO_MEM;
    CHECK(dispatch_report(2, 0, 4, 1) && callback_side_effects == 0);
    pump(4); CHECK(kms_usb_devices_active() == 0 && received == 0 && mistakes == 0);
    reject_report = false; fake_now += KMS_USB_ALLOC_RETRY_MS; pump(20);
    CHECK(kms_usb_devices_active() == 2 && mistakes == 0); // Deferred errors retain the allocation retry fix.

    reset(true); fake_now = UINT32_MAX - 30;
    announce(1, 0, 9, 1); announce(2, 1, 3, 1); pump(12);
    captured_at = fake_now; CHECK(dispatch_report(2, 0, 4, 1)); fake_now += KMS_USB_MAX_AGE_MS - 1;
    kms_usb_devices_tick();
    CHECK(received == 1 && last_captured_at == captured_at && raw_resets == 0 && mistakes == 0);
    // At the full channel budget, one failed endpoint must not strand the other
    // receiver. Queued copies from the failed activation must be invalidated.
    const int statuses[] = {USB_TRANSFER_STATUS_ERROR, USB_TRANSFER_STATUS_CANCELED, USB_TRANSFER_STATUS_NO_DEVICE};
    for (unsigned status = 0; status < sizeof(statuses) / sizeof(statuses[0]); ++status)
        for (unsigned endpoint = 0; endpoint < 4; ++endpoint)
            for (unsigned wrap = 0; wrap < 2; ++wrap) {
                reset(true); if (wrap) fake_now = UINT32_MAX - 100;
                announce(1, 0, 9, 1); announce(2, 1, 3, 2); announce(3, 1, 3, 2);
                fake_devices[3].desc.idProduct = 0x4321; pump(20);
                unsigned address = 2 + endpoint / 2, number = endpoint % 2;
                unsigned other = address == 2 ? 3 : 2;
                CHECK(dispatch_report(address, number, 4, number ? 2 : 1));
                for (unsigned n = 0; n < 32; ++n) {
                    fake_transfer_t *f = &fake_transfers[n];
                    if (!f->used || !f->inflight || f->t.device_handle != &fake_devices[address] ||
                        f->t.bEndpointAddress != 0x81 + number) continue;
                    f->t.status = statuses[status]; f->t.actual_num_bytes = 0; f->finish = true;
                }
                pump(4); kms_usb_devices_snapshot(&s);
                CHECK(row(&s, address, number)->state == KMS_USB_ERROR);
                CHECK(received == 0 && s.stream.stale == 1 && callback_side_effects == 0);
                CHECK(row(&s, other, 0)->state == KMS_USB_ACTIVE && row(&s, other, 1)->state == KMS_USB_ACTIVE);
                CHECK(dispatch_report(other, 0, 5, 1)); kms_usb_devices_tick(); CHECK(received == 1);
                kms_usb_devices_rescan(); pump(20);
                CHECK(kms_usb_devices_active() == 4 && mistakes == 0 && inline_reports == 0);
            }
    return 0;
}
