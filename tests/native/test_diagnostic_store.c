// Exercise the production worker synchronously against bounded fake queues/NVS.
#include "../../firmware/components/kms_runtime/diagnostic_store.c"
#include <stdarg.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, queue_count, now_ms, wrong_partition, event_lines;
static bool fail_storage, test_history_blocked;
static unsigned flash_writes;
static unsigned task_allocations, task_queues;
static bool fail_task_memory, fail_task_start;
static unsigned deleted_queues;
static struct { uint64_t align; uint8_t bytes[70000]; } task_heap;
void *kms_memory_calloc(size_t count, size_t bytes)
{
    ++task_allocations;
    if (fail_task_memory) return NULL;
    if (!count || bytes > sizeof(task_heap.bytes) / count) return NULL;
    memset(task_heap.bytes, 0, count * bytes); return task_heap.bytes;
}
QueueHandle_t kms_memory_queue_create(unsigned count, unsigned bytes)
{ ++task_queues; return xQueueCreate(count, bytes); }
void kms_memory_queue_delete(QueueHandle_t queue) { ++deleted_queues; vQueueDelete(queue); }
void free(void *pointer) { (void)pointer; }
bool kms_test_mode_history_allowed(uint32_t now) { (void)now; return !test_history_blocked; }
typedef struct { unsigned depth, size, count, next; uint8_t bytes[20][sizeof(job_t)]; } fake_queue_t;
static fake_queue_t queues[2];
static uint8_t flash_records[KMS_DIAGNOSTICS_EVENTS + KMS_DIAGNOSTICS_SNAPSHOTS][sizeof(kms_diag_stats_t)];
static size_t flash_sizes[KMS_DIAGNOSTICS_EVENTS + KMS_DIAGNOSTICS_SNAPSHOTS];
static bool equal(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
uint32_t kms_now(void) { return now_ms; }
unsigned esp_reset_reason(void) { return 1; }
unsigned esp_get_free_heap_size(void) { return 100000; }
unsigned heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 80000; }
void kms_fake_log(const char *tag, const char *fmt, ...)
{ (void)tag; if (fmt[0] == 'E' && fmt[1] == 'v') ++event_lines; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "fake"; }
int snprintf(char *out, size_t capacity, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    char prefix = fmt[1] == 's' ? *(va_arg(args, const char *)) : (char)va_arg(args, int);
    unsigned value = va_arg(args, unsigned); va_end(args);
    char digits[10]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    unsigned size = n < 2 ? 3 : n + 1;
    if (size + 1 > capacity) return -1;
    out[0] = prefix; unsigned at = 1; if (n < 2) out[at++] = '0';
    while (n) out[at++] = digits[--n]; out[at] = 0; return (int)at;
}
static unsigned record_index(const char *key)
{
    unsigned index = 0; for (const char *c = key + 1; *c; ++c) index = index * 10 + (unsigned)(*c - '0');
    return index + (key[0] == 's' ? KMS_DIAGNOSTICS_EVENTS : 0);
}
esp_err_t nvs_flash_init_partition(const char *name)
{ if (!equal(name, "kms_diag")) ++wrong_partition; return fail_storage ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode, nvs_handle_t *h)
{
    (void)ns; (void)mode; *h = 1;
    if (!equal(partition, "kms_diag")) ++wrong_partition;
    return fail_storage ? ESP_FAIL : ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    (void)h; unsigned index = record_index(key);
    if (!flash_sizes[index]) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < flash_sizes[index]) return ESP_ERR_NVS_INVALID_LENGTH;
    *size = flash_sizes[index]; memcpy(out, flash_records[index], *size); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    (void)h; if (fail_storage) return ESP_FAIL;
    ++flash_writes;
    unsigned index = record_index(key); memcpy(flash_records[index], data, size); flash_sizes[index] = size; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return fail_storage ? ESP_FAIL : ESP_OK; }
QueueHandle_t xQueueCreate(unsigned depth, unsigned size)
{
    if (queue_count >= 2 || depth > 20 || size > sizeof(job_t)) return 0;
    fake_queue_t *q = &queues[queue_count++]; memset(q, 0, sizeof(*q)); q->depth = depth; q->size = size; return q;
}
int xQueueSend(QueueHandle_t handle, const void *data, uint32_t wait)
{
    (void)wait; fake_queue_t *q = handle; if (q->count == q->depth) return 0;
    unsigned at = (q->next + q->count++) % q->depth; memcpy(q->bytes[at], data, q->size); return pdTRUE;
}
int xQueueReceive(QueueHandle_t handle, void *data, uint32_t wait)
{
    (void)wait; fake_queue_t *q = handle; if (!q->count) return 0;
    memcpy(data, q->bytes[q->next], q->size); q->next = (q->next + 1) % q->depth; --q->count; return pdTRUE;
}
void vQueueDelete(QueueHandle_t handle) { (void)handle; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return fail_task_start ? 0 : pdPASS; }
static void pump(void)
{ job_t job; while (xQueueReceive(jobs, &job, 0)) process_job(&job); kms_diagnostics_tick(); }
static void reset_ram(void)
{
    if (storage) free(storage);
    storage = NULL;
    jobs = receipts = NULL;
    task_allocations = task_queues = deleted_queues = 0;
    stat_index = scan_index = queue_count = storage_errors = 0; initialized = false;
}
__declspec(dllexport) int kms_run_store_tests(void)
{
    checks = 0; reset_ram(); fail_task_memory = true; kms_diagnostics_init(50);
    CHECK(!initialized && !storage && !jobs && !receipts);
    kms_diagnostics_event(KMS_EVENT_SELECTED, 1, 0); kms_diagnostics_tick(); kms_diagnostics_save();
    kms_diag_record_t unavailable;
    kms_diagnostics_capture_event(&unavailable, KMS_EVENT_INPUT_ERROR, 2, 3);
    CHECK(unavailable.boot == 50 && unavailable.slot == 2 && unavailable.detail == 3 && !unavailable.idle_ms);
    fail_task_memory = false; reset_ram(); fail_task_start = true; kms_diagnostics_init(51);
    CHECK(initialized && storage && !jobs && !receipts && deleted_queues == 2);
    CHECK(storage->model.next_sequence == 1 && storage->model.stats.storage_errors == 1);
    fail_task_start = false; reset_ram(); kms_diagnostics_init(55); pump();
    CHECK(task_allocations == 1 && task_queues == 2);
    CHECK(queues[0].depth == KMS_DIAGNOSTICS_QUEUE_DEPTH);
    CHECK(queues[1].depth == KMS_DIAGNOSTICS_QUEUE_DEPTH + 2);
    // Saturate the worker without stopping RAM logging or the input loop.
    for (unsigned n = 0; n < KMS_DIAGNOSTICS_EVENTS + 40; ++n)
        kms_diagnostics_event(KMS_EVENT_SELECTED, 2, n);
    CHECK(queues[0].count == KMS_DIAGNOSTICS_QUEUE_DEPTH);
    CHECK(storage->model.next_sequence == KMS_DIAGNOSTICS_EVENTS + 41);
    unsigned before = scan_index; kms_diagnostics_tick();
    CHECK(scan_index == (before + KMS_DIAGNOSTICS_SCAN_BATCH) % KMS_DIAGNOSTICS_EVENTS);
    for (unsigned n = 0; n < 400; ++n) { now_ms += 20; pump(); }
    for (unsigned n = 0; n < KMS_DIAGNOSTICS_EVENTS; ++n) {
        CHECK(storage->saved_sequence[n] == storage->model.records[n].sequence);
        CHECK(flash_sizes[n] == sizeof(kms_diag_record_t));
    }
    uint32_t last = storage->model.next_sequence; event_lines = 0; dump_range(&storage->model.stats, false, now_ms, 0, 0);
    CHECK(event_lines == KMS_DIAGNOSTICS_EVENTS);
    // Actual failed writes produce receipts, retain RAM and retry successfully.
    fail_storage = true; kms_diagnostics_event(KMS_EVENT_USB_STALL, 2, 99); pump();
    CHECK(storage->model.stats.storage_errors > 0);
    fail_storage = false;
    for (unsigned n = 0; n < 200; ++n) { now_ms += 20; pump(); }
    unsigned at = (storage->model.next + KMS_DIAGNOSTICS_EVENTS - 1) % KMS_DIAGNOSTICS_EVENTS;
    CHECK(storage->saved_sequence[at] == last + 1);
    for (unsigned n = 0; n < KMS_DIAGNOSTICS_SNAPSHOTS + 3; ++n) { kms_diagnostics_save(); pump(); }
    CHECK(storage->model.stats.sequence == KMS_DIAGNOSTICS_SNAPSHOTS + 3);
    for (unsigned n = 0; n < KMS_DIAGNOSTICS_SNAPSHOTS; ++n)
        CHECK(flash_sizes[KMS_DIAGNOSTICS_EVENTS + n] == sizeof(kms_diag_stats_t));
    // Reboot loads only the new dedicated partition and resumes the ring.
    reset_ram(); kms_diagnostics_init(66);
    CHECK(storage->model.next_sequence == last + 2);
    CHECK(storage->model.stats.sequence == KMS_DIAGNOSTICS_SNAPSHOTS + 3);
    CHECK(wrong_partition == 0);
    pump();
    kms_diag_record_t fault;
    kms_diagnostics_capture_event(&fault, KMS_EVENT_INPUT_ERROR, 2,
        KMS_USB_HOST_EVENT_TAG | KMS_USB_HOST_CALL_TIMEOUT << 16 | 3u << 8);
    CHECK(fault.boot == 66 && fault.uptime_ms == now_ms && fault.slot == 2);
    fault.uptime_ms += 3000; fault.idle_ms += 3000;
    reset_ram(); kms_diagnostics_init(77); pump();
    kms_diagnostics_recover_event(&fault); pump();
    at = (storage->model.next + KMS_DIAGNOSTICS_EVENTS - 1) % KMS_DIAGNOSTICS_EVENTS;
    CHECK(storage->model.records[at].boot == 66 && storage->model.records[at].uptime_ms == fault.uptime_ms);
    CHECK(storage->model.records[at].idle_ms == fault.idle_ms && storage->model.records[at].detail == fault.detail);
    CHECK(equal(kms_diag_event_name(storage->model.records[at].event, storage->model.records[at].detail), "USB call blocked"));
    kms_diag_record_t persisted;
    memcpy(&persisted, flash_records[at], sizeof(persisted));
    CHECK(persisted.boot == 66 && persisted.uptime_ms == fault.uptime_ms && persisted.sequence == storage->model.next_sequence);
    reset_ram(); kms_diagnostics_init(88);
    CHECK(storage->model.records[at].boot == 66 && storage->model.records[at].detail == fault.detail);
    pump(); unsigned written = flash_writes;
    test_history_blocked = true;
    for (unsigned n = 0; n < 200; ++n) {
        kms_diagnostics_event(KMS_EVENT_SELECTED, 2, n); pump();
    }
    CHECK(flash_writes == written); // Test history still accumulates in RAM.
    test_history_blocked = false;
    for (unsigned n = 0; n < 400; ++n) { now_ms += 20; pump(); }
    CHECK(flash_writes == written); // Suppressed test records do not flush as a burst on exit.
    reset_ram(); fail_storage = true; kms_diagnostics_init(99);
    CHECK(initialized && storage->model.stats.storage_errors > 0 && storage->model.next_sequence == 1);
    fail_storage = false;
    reset_ram(); now_ms = 40u * 60000u; kms_diagnostics_init(77); pump();
    memset(storage->model.records, 0, sizeof(storage->model.records)); memset(storage->snapshots, 0, sizeof(storage->snapshots));
    storage->model.next = 0; storage->model.next_sequence = 4;
    for (unsigned n = 0; n < 4; ++n) storage->model.records[n] = (kms_diag_record_t){
        .version = STORE_VERSION, .sequence = n + 1, .boot = n == 3 ? 76 : 77,
        .uptime_ms = now_ms - (n + 1) * 10u * 60000u};
    event_lines = 0; dump_range(&storage->model.stats, true, now_ms, 20u * 60000u, 30u * 60000u);
    CHECK(event_lines == 2);
    event_lines = 0; dump_range(&storage->model.stats, true, now_ms, 0, 10u * 60000u);
    CHECK(event_lines == 1);
    event_lines = 0; dump_range(&storage->model.stats, false, now_ms, 0, 0);
    CHECK(event_lines == 4);
    CHECK(in_range(77, 1200000, &storage->model.stats, 1200000, 1800000));
    CHECK(!in_range(76, 1200000, &storage->model.stats, 1200000, 1800000));
    CHECK(!in_range(77, 1199999, &storage->model.stats, 1200000, 1800000));
    CHECK(!in_range(77, 1800001, &storage->model.stats, 1200000, 1800000));
    static const char *invalid[] = {"history -1", "history 1.5", "history 1 2 3", "history 71583", "history 999999999999999999", "history 10x"};
    for (unsigned n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
        unsigned count = queues[0].count; CHECK(kms_diagnostics_console(invalid[n])); CHECK(queues[0].count == count);
    }
    CHECK(!kms_diagnostics_console("historyx"));
    CHECK(kms_diagnostics_console("history 20 30"));
    job_t job; CHECK(xQueueReceive(jobs, &job, 0) == pdTRUE);
    CHECK(job.type == DUMP && job.filtered && job.youngest_ms == 1200000 && job.oldest_ms == 1800000 && job.now == now_ms);
    CHECK(kms_diagnostics_console("history 30 20")); CHECK(xQueueReceive(jobs, &job, 0) == pdTRUE);
    CHECK(job.youngest_ms == 1200000 && job.oldest_ms == 1800000);
    CHECK(kms_diagnostics_console("history 10")); CHECK(xQueueReceive(jobs, &job, 0) == pdTRUE);
    CHECK(job.youngest_ms == 0 && job.oldest_ms == 600000);
    CHECK(kms_diagnostics_console("history")); CHECK(xQueueReceive(jobs, &job, 0) == pdTRUE); CHECK(!job.filtered);
    CHECK(kms_diagnostics_console("history 0")); CHECK(xQueueReceive(jobs, &job, 0) == pdTRUE); CHECK(job.filtered && !job.oldest_ms);
    const char *maximum = "71582"; uint32_t minutes;
    CHECK(parse_minutes(&maximum, &minutes) && minutes == 71582 && !*maximum);
    return 0;
}
__declspec(dllexport) unsigned kms_store_test_count(void) { return checks; }
