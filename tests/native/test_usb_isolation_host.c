// Execute production SDK callbacks, ownership and frozen snapshot logic.
#define ISO_DRIVER_TRACE 1
#define app_main iso_app_main
#define esp_timer_get_time iso_test_time
#include "../../diagnostics/usb-isolation/main/main.c"
static unsigned checks, assertions, allocated, freed, closes, releases, submits, controls;
static iso_trace_store_t trace_test_store;
static unsigned trace_captures;
void iso_driver_reset(void) { iso_trace_reset(&trace_test_store); }
void iso_driver_print(void) {}
void iso_hcd_capture(bool freeze)
{
    ++trace_captures;
    iso_trace_sample_t sample = {.at = 123}; iso_trace_sample(&trace_test_store, &sample);
    if (freeze) iso_trace_freeze(&trace_test_store);
}
static unsigned fail_alloc, fail_claim;
static uint32_t test_time;
static usb_transfer_t transfers[4];
static uint8_t buffers[4][64];
static bool used[4], busy[4];
usb_dev_t USB0;
int64_t iso_test_time(void) { return (int64_t)test_time * 1000; }
void kms_host_assert(int value) { if (!value) ++assertions; }
QueueHandle_t xQueueCreate(unsigned n, unsigned bytes) { (void)n; (void)bytes; return (void *)1; }
int xQueueSend(QueueHandle_t q, const void *p, uint32_t wait) { (void)q; (void)p; (void)wait; return pdTRUE; }
int xQueueReceive(QueueHandle_t q, void *p, uint32_t wait) { (void)q; (void)p; (void)wait; return pdFALSE; }
int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *out, int cpu)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)out; (void)cpu; return pdPASS; }
void vTaskDelay(unsigned ticks) { test_time += ticks; }
esp_err_t usb_host_install(const usb_host_config_t *c) { (void)c; return ESP_OK; }
esp_err_t usb_host_client_register(const usb_host_client_config_t *c, usb_host_client_handle_t *h)
{ (void)c; *h = (void *)1; return ESP_OK; }
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t c, int ticks) { (void)c; (void)ticks; return ESP_OK; }
esp_err_t usb_host_lib_handle_events(int ticks, uint32_t *flags) { (void)ticks; *flags = 0; return ESP_OK; }
esp_err_t usb_host_device_open(usb_host_client_handle_t c, uint8_t a, usb_device_handle_t *h)
{ (void)c; *h = (void *)(uintptr_t)a; return ESP_OK; }
esp_err_t usb_host_device_close(usb_host_client_handle_t c, usb_device_handle_t h)
{ (void)c; (void)h; ++closes; return ESP_OK; }
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t h, const usb_device_desc_t **out)
{ (void)h; static usb_device_desc_t d = {.idVendor = KMS_GEN_VID, .idProduct = KMS_GEN_PID}; *out = &d; return ESP_OK; }
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t h, const usb_config_desc_t **out)
{
    (void)h;
    static const uint8_t data[] = {9,2,34,0,1,1,0,0x80,50, 9,4,0,0,1,3,0,0,0,
                                   9,0x21,0x11,1,0,1,0x22,100,0, 7,5,0x81,3,64,0,1};
    *out = (const usb_config_desc_t *)data; return ESP_OK;
}
esp_err_t usb_host_device_info(usb_device_handle_t h, usb_device_info_t *info)
{
    (void)h;
    static const uint8_t serial[] = {26,3,'A',0,'A',0,'B',0,'B',0,'C',0,'C',0,'D',0,'D',0,'E',0,'E',0,'0',0,'1',0};
    memset(info, 0, sizeof(*info)); info->str_desc_serial_num = (const usb_str_desc_t *)serial; return ESP_OK;
}
esp_err_t usb_host_transfer_alloc(size_t bytes, int packets, usb_transfer_t **out)
{
    (void)packets; ++allocated;
    if (allocated == fail_alloc) return ESP_ERR_NO_MEM;
    for (unsigned n = 0; n < 4; ++n) if (!used[n]) {
        used[n] = true; transfers[n] = (usb_transfer_t){.data_buffer = buffers[n], .data_buffer_size = bytes};
        *out = &transfers[n]; return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}
esp_err_t usb_host_transfer_free(usb_transfer_t *t)
{
    unsigned n = (unsigned)(t - transfers); kms_host_assert(n < 4 && used[n] && !busy[n]);
    used[n] = false; ++freed; return ESP_OK;
}
esp_err_t usb_host_interface_claim(usb_host_client_handle_t c, usb_device_handle_t h, uint8_t i, uint8_t a)
{ (void)c; (void)h; (void)i; (void)a; return fail_claim ? ESP_ERR_NO_MEM : ESP_OK; }
esp_err_t usb_host_interface_release(usb_host_client_handle_t c, usb_device_handle_t h, uint8_t i)
{ (void)c; (void)h; (void)i; ++releases; return ESP_OK; }
esp_err_t usb_host_transfer_submit(usb_transfer_t *t)
{ unsigned n = (unsigned)(t - transfers); kms_host_assert(used[n] && !busy[n]); busy[n] = true; ++submits; return ESP_OK; }
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t c, usb_transfer_t *t)
{ (void)c; unsigned n = (unsigned)(t - transfers); kms_host_assert(used[n] && !busy[n]); busy[n] = true; ++controls; return ESP_OK; }
esp_err_t usb_host_endpoint_halt(usb_device_handle_t h, uint8_t e) { (void)h; (void)e; return ESP_OK; }
esp_err_t usb_host_endpoint_flush(usb_device_handle_t h, uint8_t e) { (void)h; (void)e; return ESP_OK; }
static void complete(usb_transfer_t *t, int status, int bytes)
{ busy[t - transfers] = false; t->status = status; t->actual_num_bytes = bytes; t->callback(t); }
static void reset(void)
{
    memset(devices, 0, sizeof(devices)); memset(used, 0, sizeof(used)); memset(busy, 0, sizeof(busy));
    memset(&frozen, 0, sizeof(frozen)); memset(&USB0, 0, sizeof(USB0));
    run = seed = profile = mode = expected = fault = watch_start = 0; watching = false;
    sample_count = sample_next = 0; test_time = 100;
    iso_driver_reset(); trace_captures = 0;
    allocated = freed = closes = releases = submits = controls = fail_alloc = fail_claim = assertions = 0;
}
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_run_isolation_host_tests(void)
{
    checks = 0;
    reset(); iso_guard_stats_t guard_state;
    CHECK(execute((command_t){.kind=CMD_WORKAROUND,.workaround=2})!=0);
    CHECK(execute((command_t){.kind=CMD_WORKAROUND,.workaround=0})==0);
    iso_guard_snapshot(&guard_state); CHECK(!guard_state.enabled);
    watching=true; CHECK(execute((command_t){.kind=CMD_WORKAROUND,.workaround=1})!=0);
    iso_guard_snapshot(&guard_state); CHECK(!guard_state.enabled);
    watching=false; CHECK(execute((command_t){.kind=CMD_WORKAROUND,.workaround=1})==0);
    snapshot_t mode_snapshot; snapshot(&mode_snapshot); CHECK(mode_snapshot.guard.enabled==1);
    for (unsigned selected_mode = 0; selected_mode <= 3; ++selected_mode) {
        reset(); attach(1); device_t *d = &devices[0];
        CHECK(d->claimed && d->input && d->control && !d->state.pending);
        CHECK(d->input->data_buffer && d->input->data_buffer_size == 64);
        CHECK(execute((command_t){.kind = CMD_ARM, .run = 123, .seed = 1, .profile = 1, .mode = selected_mode, .expected = 1}) == 0);
        CHECK(execute((command_t){.kind = CMD_WATCH, .watching = 1}) == 0);
        device_tick(d, 100); CHECK(d->state.pending && submits == 1);
        device_tick(d, 1100); CHECK(controls == (selected_mode ? 1u : 0u));
        if (selected_mode) {
            usb_setup_packet_t *setup = (usb_setup_packet_t *)d->control->data_buffer;
            CHECK(setup->wLength == 2 && d->control->num_bytes == 10);
            CHECK(setup->bmRequestType == (selected_mode == 2 ? 0x21 : 0x80));
            if (selected_mode == 2) CHECK(setup->wValue == 0x0201 && d->control->data_buffer[8] == 1);
            complete(d->control, USB_TRANSFER_STATUS_COMPLETED, 10);
            CHECK(!d->state.control_pending && d->state.controls == 1 && !fault);
        }
        device_tick(d, 10101); CHECK(fault == FAULT_INPUT);
        CHECK(trace_test_store.frozen && trace_captures == 1 && trace_test_store.sample_count == 1);
        snapshot_t first = frozen; freeze(FAULT_CONTROL);
        CHECK(trace_captures == 1);
        CHECK(!memcmp(&frozen, &first, sizeof(first))); // First fault is immutable.
        CHECK(execute((command_t){.kind = CMD_ARM, .run = 456, .expected = 1}) != 0);
        usb_host_client_event_msg_t event = {.event = USB_HOST_CLIENT_EVENT_DEV_GONE, .dev_gone = {.dev_hdl = d->handle}};
        device_event(&event, NULL); retire(d); CHECK(!freed && !closes);
        complete(d->input, USB_TRANSFER_STATUS_NO_DEVICE, 0); retire(d);
        CHECK(freed == 2 && closes == 1 && releases == 1 && !assertions);
    }
    // Incomplete control cancellation keeps both buffers owned, even when the
    // input cancellation has drained. No SDK buffer is freed early.
    reset(); attach(1); device_t *d = &devices[0]; watching = true; mode = 1;
    device_tick(d, 1100); CHECK(d->state.pending && d->state.control_pending);
    usb_host_client_event_msg_t gone = {.event = USB_HOST_CLIENT_EVENT_DEV_GONE, .dev_gone = {.dev_hdl = d->handle}};
    device_event(&gone, NULL); complete(d->input, USB_TRANSFER_STATUS_NO_DEVICE, 0); retire(d); CHECK(freed == 0);
    complete(d->control, USB_TRANSFER_STATUS_NO_DEVICE, 0); retire(d); CHECK(freed == 2 && !assertions);
    for (unsigned stage = 1; stage <= 3; ++stage) {
        reset(); if (stage <= 2) fail_alloc = stage; else fail_claim = 1;
        attach(1); CHECK(!devices[0].handle && closes == 1);
        CHECK(freed == (stage == 1 ? 0u : stage == 2 ? 1u : 2u) && !assertions);
    }
    // Control and input watchdogs remain correct across the uint32 clock wrap.
    reset(); attach(1); d = &devices[0]; watching = true; watch_start = 0xfffffff0u;
    d->state.control_pending = 1; d->state.control_start = 0xfffffff0u;
    device_tick(d, 0x3d7u); CHECK(!fault);
    device_tick(d, 0x3d8u); CHECK(fault == FAULT_CONTROL && frozen.frozen == 1);
    return 0;
}
__declspec(dllexport) unsigned kms_isolation_host_check_count(void) { return checks; }
