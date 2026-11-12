// Production console snapshots retain ownership while UART output is delayed.
#include <stddef.h>
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
#include "usb_console.c"
#include <stdarg.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, mistakes, snapshot_calls, print_calls, queued_commands;
static uint32_t begin_tokens[2], end_tokens[2], frame_boot, frame_at, frame_fault;
static unsigned begins, ends;
static unsigned guard_prints, guard_waited;
static kms_usb_fault_snapshot_t fault;
static unsigned task_allocations;
static bool fail_task_memory;
static struct { uint64_t align; uint8_t bytes[2 * sizeof(console_job_t)]; } task_heap;
void *kms_memory_calloc(size_t count, size_t bytes)
{
    ++task_allocations;
    if (fail_task_memory) return NULL;
    if (!count || bytes > sizeof(task_heap.bytes) / count) return NULL;
    memset(task_heap.bytes, 0, count * bytes); return task_heap.bytes;
}
void free(void *pointer) { if (pointer != task_heap.bytes) ++mistakes; }
typedef struct { void *items[2]; unsigned capacity, head, count; } queue_t;
static queue_t queues[2];
static unsigned queue_count;
QueueHandle_t xQueueCreate(unsigned count, unsigned bytes)
{
    if (queue_count >= 2 || count != 2 || bytes != sizeof(void *)) { ++mistakes; return NULL; }
    queue_t *q = &queues[queue_count++]; memset(q, 0, sizeof(*q)); q->capacity = count; return q;
}
int xQueueSend(QueueHandle_t handle, const void *value, uint32_t wait)
{
    queue_t *q = handle; if (wait) ++mistakes;
    if (q->count == q->capacity) return pdFALSE;
    q->items[(q->head + q->count) % q->capacity] = *(void *const *)value; ++q->count; return pdTRUE;
}
int xQueueReceive(QueueHandle_t handle, void *value, uint32_t wait)
{
    queue_t *q = handle; if (wait) ++mistakes;
    if (!q->count) return pdFALSE;
    *(void **)value = q->items[q->head]; q->head = (q->head + 1) % q->capacity; --q->count; return pdTRUE;
}
void vQueueDelete(QueueHandle_t handle) { (void)handle; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)name; (void)stack; (void)arg; (void)handle; if (task != list_worker || priority != 1) ++mistakes; return pdPASS; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; ++print_calls; }
int printf(const char *format, ...)
{
    ++print_calls;
    va_list args; va_start(args, format);
    if (!strncmp(format, "KMSUSBSTATUS BEGIN ", 19)) {
        if (begins < 2) begin_tokens[begins] = va_arg(args, uint32_t);
        ++begins; frame_boot = va_arg(args, uint32_t); frame_at = va_arg(args, uint32_t);
        (void)va_arg(args, unsigned); frame_fault = va_arg(args, uint32_t);
    } else if (!strncmp(format, "KMSUSBTRACE WORKAROUND ", 23)) {
        ++guard_prints;
        if (va_arg(args, uint32_t) != 1) ++mistakes;
        if (va_arg(args, unsigned) != 20 || va_arg(args, unsigned) != 64) ++mistakes;
        if (va_arg(args, uint32_t) != 42) ++mistakes;
        guard_waited = va_arg(args, uint32_t);
    } else if (!strncmp(format, "KMSUSBSTATUS END ", 17)) {
        if (ends < 2) end_tokens[ends] = va_arg(args, uint32_t);
        ++ends;
    }
    va_end(args); return 0;
}
int snprintf(char *out, size_t capacity, const char *format, ...)
{ (void)format; if (capacity) *out = 0; return 0; }
int sscanf(const char *text, const char *format, ...) { (void)text; (void)format; return 0; }
int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t bytes)
{ while (bytes && *a && *a == *b) { ++a; ++b; --bytes; } return bytes ? (unsigned char)*a - (unsigned char)*b : 0; }
bool kms_usb_input_command(const char *line) { (void)line; ++queued_commands; return true; }
unsigned kms_usb_input_sources(void) { return 3; }
unsigned kms_usb_input_selected(void) { return 3; }
uint32_t kms_now(void) { return 1234; }
void kms_usb_input_capture_event(kms_diag_record_t *out, enum kms_event e, unsigned slot, uint32_t detail)
{ *out = (kms_diag_record_t){.boot = 0x12345678, .event = (uint16_t)e, .slot = (uint16_t)slot, .detail = detail}; }
void kms_usb_devices_snapshot(kms_usb_inventory_t *out)
{
    ++snapshot_calls; memset(out, 0, sizeof(*out)); out->controls = 3; out->device_count = 1; out->devices[0].address = 7;
    out->stream.callback.max_us = 17; out->stream.callback.at = 123;
    out->stream.recent_count = 1; out->stream.recent[0] = (kms_usb_delay_t){.at = 120, .gap_us = 6500};
}
void kms_usb_leds_snapshot(kms_usb_led_status_t *out) { ++snapshot_calls; memset(out, 0, sizeof(*out)); out->enabled = true; }
void kms_usb_health_status_snapshot(kms_usb_health_status_t *out) { ++snapshot_calls; memset(out, 0, sizeof(*out)); out->pending = 2; }
void kms_usb_recovery_status_snapshot(kms_usb_recovery_status_t *out) { ++snapshot_calls; memset(out, 0, sizeof(*out)); out->recoveries = 4; }
void kms_usb_leds_print_status(const kms_usb_led_status_t *s) { (void)s; ++print_calls; }
void kms_usb_health_print_status(const kms_usb_health_status_t *s) { (void)s; ++print_calls; }
void kms_usb_recovery_print_status(const kms_usb_recovery_status_t *s) { (void)s; ++print_calls; }
void kms_usb_memory_snapshot(kms_usb_memory_status_t *out) { memset(out, 0, sizeof(*out)); }
void kms_usb_memory_print(const kms_usb_memory_status_t *s) { (void)s; ++print_calls; }
esp_err_t usb_host_lib_info(usb_host_lib_info_t *out) { memset(out, 0, sizeof(*out)); return ESP_OK; }
const char *kms_usb_interface_state(unsigned state) { (void)state; return "active"; }
const kms_usb_fault_snapshot_t *kms_usb_fault_latest(void) { return &fault; }
void kms_usb_bus_snapshot(kms_usb_bus_snapshot_t *out) { memset(out, 0, sizeof(*out)); }
void kms_usb_bus_history(kms_usb_bus_history_t *out) { memset(out, 0, sizeof(*out)); }
void kms_usb_driver_snapshot(kms_usb_driver_snapshot_t *out)
{ memset(out, 0, sizeof(*out)); out->frame_guard = (kms_usb_frame_guard_stats_t){.enabled = 1, .activations = 42, .waited = 7}; }
void kms_usb_devices_rescan(void) { }
bool kms_usb_devices_select(unsigned a, unsigned n, bool enabled) { (void)a; (void)n; (void)enabled; return true; }
bool kms_usb_devices_automatic(bool enabled) { (void)enabled; return true; }
bool kms_usb_recovery_request(void) { return true; }
__declspec(dllexport) unsigned kms_usb_console_test_count(void) { return checks; }
__declspec(dllexport) int kms_run_usb_console_tests(void)
{
    fail_task_memory = true; kms_usb_console_init();
    CHECK(!jobs && !lists && !available && !queue_count);
    CHECK(!kms_usb_console_fault_queue(&fault));
    fail_task_memory = false; task_allocations = 0;
    kms_usb_console_init(); CHECK(queue_count == 2 && queues[1].count == 2);
    CHECK(task_allocations == 1);
    fault.sequence = 1; fault.reason.address = 23;
    CHECK(kms_usb_console_fault_queue(&fault)); fault.sequence = 2; fault.reason.address = 24;
    CHECK(kms_usb_console_fault_queue(&fault)); fault.sequence = 3;
    CHECK(!kms_usb_console_fault_queue(&fault));
    console_job_t *first, *second;
    CHECK(xQueueReceive(lists, &first, 0) == pdTRUE);
    CHECK(first->data.snapshot.sequence == 1 && first->data.snapshot.reason.address == 23);
    CHECK(!kms_usb_console_fault_queue(&fault)); // A slow printer still owns the dequeued buffer.
    CHECK(xQueueReceive(lists, &second, 0) == pdTRUE);
    CHECK(second != first && second->data.snapshot.sequence == 2 && second->data.snapshot.reason.address == 24);
    CHECK(xQueueSend(available, &first, 0) == pdTRUE);
    CHECK(kms_usb_console_fault_queue(&fault));
    CHECK(second->data.snapshot.sequence == 2); // Reusing the released buffer cannot alter the other job.
    CHECK(xQueueSend(available, &second, 0) == pdTRUE);
    CHECK(xQueueReceive(lists, &first, 0) == pdTRUE && first->data.snapshot.sequence == 3);
    CHECK(xQueueSend(available, &first, 0) == pdTRUE);
    CHECK(kms_usb_console_execute("usb-status"));
    CHECK(xQueueReceive(lists, &first, 0) == pdTRUE && first->kind == CONSOLE_STATUS);
    CHECK(first->data.status.inventory.devices[0].address == 7 && first->data.status.sources == 3);
    CHECK(first->data.status.health.pending == 2 && first->data.status.recovery.recoveries == 4);
    CHECK(first->data.status.inventory.stream.callback.max_us == 17 && first->data.status.inventory.stream.callback.at == 123);
    CHECK(first->data.status.inventory.stream.recent_count == 1 && first->data.status.inventory.stream.recent[0].gap_us == 6500);
    unsigned before = snapshot_calls; print_job(first);
    CHECK(print_calls >= 5 && snapshot_calls == before); // Printing touches copies, never the live USB owner.
    CHECK(!begins && !ends); // Legacy human-readable status is unchanged.
    CHECK(xQueueSend(available, &first, 0) == pdTRUE);
    CHECK(!kms_usb_console_command("status")); CHECK(kms_usb_console_command("usb-list"));
    CHECK(queued_commands == 1 && queues[0].count == 0); // Main only queues work, it cannot touch USB state.
    CHECK(mistakes == 0);
    CHECK(kms_usb_console_execute("usb-status 4294967295"));
    CHECK(xQueueReceive(lists, &second, 0) == pdTRUE);
    CHECK(second->data.status.token == UINT32_MAX && second->data.status.context.boot == 0x12345678);
    CHECK(second->data.status.context.uptime_ms == 1234 && second->data.status.context.slot == 3);
    CHECK(second->data.status.fault_sequence == 3);
    uint32_t frozen_token = second->data.status.token;
    CHECK(kms_usb_console_execute("usb-status 7"));
    CHECK(second->data.status.token == frozen_token); // A queued snapshot cannot change under the worker.
    CHECK(xQueueReceive(lists, &first, 0) == pdTRUE && first->data.status.token == 7);
    print_job(second); (void)xQueueSend(available, &second, 0);
    print_job(first); (void)xQueueSend(available, &first, 0);
    CHECK(begins == 2 && ends == 2 && begin_tokens[0] == UINT32_MAX && begin_tokens[1] == 7);
    CHECK(end_tokens[0] == begin_tokens[0] && end_tokens[1] == begin_tokens[1]);
    CHECK(frame_boot == 0x12345678 && frame_at == 1234 && frame_fault == 3);
    const char *invalid[] = {"usb-status 0", "usb-status -1", "usb-status 4294967296", "usb-status 2x", "usb-status ", "usb-status 1 extra"};
    for (unsigned n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
        unsigned previous_calls = snapshot_calls;
        CHECK(kms_usb_console_execute(invalid[n]) && snapshot_calls == previous_calls && queues[0].count == 0);
    }
    CHECK(kms_usb_console_execute("usb-trace"));
    CHECK(xQueueReceive(lists, &first, 0) == pdTRUE && first->kind == CONSOLE_DRIVER);
    CHECK(first->data.snapshot.driver.frame_guard.waited == 7);
    print_job(first); (void)xQueueSend(available, &first, 0);
    CHECK(guard_prints == 1 && guard_waited == 7 && mistakes == 0);
    return 0;
}
