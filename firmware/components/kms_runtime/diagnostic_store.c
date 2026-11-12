#include "kms_diagnostic_store.h"
#include "kms_runtime.h"
#include "kms_memory.h"
#include <stdlib.h>
#include "kms_test_mode.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

// Diagnostic errors must never erase pairing data or abort input forwarding.
enum { WRITE_EVENT, WRITE_STATS, DUMP, STORE_VERSION = 1 };
typedef struct {
    unsigned type, index;
    bool filtered;
    uint32_t now, youngest_ms, oldest_ms;
    union { kms_diag_record_t event; kms_diag_stats_t stats; } data;
} job_t;
typedef struct { unsigned index; uint32_t sequence, repeats; bool success; } receipt_t;
typedef struct {
    kms_diag_model_t model;
    kms_diag_stats_t snapshots[KMS_DIAGNOSTICS_SNAPSHOTS];
    uint32_t saved_sequence[KMS_DIAGNOSTICS_EVENTS], saved_repeats[KMS_DIAGNOSTICS_EVENTS];
    uint32_t pending_sequence[KMS_DIAGNOSTICS_EVENTS], last_attempt[KMS_DIAGNOSTICS_EVENTS];
} history_storage_t;
static history_storage_t *storage;
static uint32_t boot_id;
static unsigned stat_index, scan_index;
static bool storage_ready;
static QueueHandle_t jobs;
static QueueHandle_t receipts;
static portMUX_TYPE diagnostic_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t storage_errors;
static bool initialized;
static const char *TAG = "diagnostics";

static void storage_error(esp_err_t e)
{
    portENTER_CRITICAL(&diagnostic_lock); ++storage_errors; portEXIT_CRITICAL(&diagnostic_lock);
    ESP_LOGW(TAG, "History storage failed (%s); input continues, RAM counters retained", esp_err_to_name(e));
}
static esp_err_t read_record(nvs_handle_t h, const char *prefix, unsigned index, void *out, size_t bytes)
{
    char key[12]; snprintf(key, sizeof(key), "%s%02u", prefix, index);
    size_t size = bytes; esp_err_t e = nvs_get_blob(h, key, out, &size);
    return e == ESP_OK && size != bytes ? ESP_ERR_NVS_INVALID_LENGTH : e;
}
static void print_stats(const kms_diag_stats_t *s)
{
    ESP_LOGI(TAG, "Stats #%" PRIu32 " boot=%08" PRIx32 " reset=%" PRIu32 " uptime=%" PRIu64
        "ms idle=%" PRIu64 "ms max-idle=%" PRIu64 "ms reports=%" PRIu32 " activity=%" PRIu32
        " tx=%" PRIu32 " rx=%" PRIu32 " retries=%" PRIu32 " timeouts=%" PRIu32
        " recovered=%" PRIu32 " recovery=%" PRIu64 "ms replayed=%" PRIu32
        " discarded=%" PRIu32 " peak-buffer=%" PRIu32 " storage-errors=%" PRIu32,
        s->sequence, s->boot, s->reset_reason, s->uptime_ms, s->idle_ms, s->max_idle_ms, s->reports,
        s->activity, s->radio_tx, s->radio_rx, s->retries, s->events[KMS_EVENT_ACK_TIMEOUT],
        s->events[KMS_EVENT_RECOVERED], s->recovery_ms, s->replayed, s->dropped, s->peak_buffer, s->storage_errors);
}
static bool in_range(uint32_t boot, uint64_t age, const kms_diag_stats_t *current,
                     uint32_t youngest_ms, uint32_t oldest_ms)
{
    return boot == current->boot && age >= youngest_ms && age <= oldest_ms;
}
static void dump_range(const kms_diag_stats_t *current, bool filtered, uint32_t now,
                       uint32_t youngest_ms, uint32_t oldest_ms)
{
    // Print in ring order with short locks; input and new records continue during UART output.
    portENTER_CRITICAL(&diagnostic_lock);
    unsigned event_start = storage->model.next, stats_start = stat_index;
    uint32_t last_sequence = storage->model.next_sequence;
    portEXIT_CRITICAL(&diagnostic_lock);
    ESP_LOGI(TAG, "History capacity=%u events, %u checkpoints; through event #%" PRIu32,
             KMS_DIAGNOSTICS_EVENTS, KMS_DIAGNOSTICS_SNAPSHOTS, last_sequence);
    if (filtered) ESP_LOGI(TAG, "History range=%" PRIu32 "..%" PRIu32 " minutes ago (current boot only)",
                          youngest_ms / 60000u, oldest_ms / 60000u);
    for (unsigned n = 0; n < KMS_DIAGNOSTICS_EVENTS; ++n) {
        unsigned i = (event_start + n) % KMS_DIAGNOSTICS_EVENTS;
        portENTER_CRITICAL(&diagnostic_lock); kms_diag_record_t r = storage->model.records[i]; portEXIT_CRITICAL(&diagnostic_lock);
        if (r.version != STORE_VERSION || kms_newer(r.sequence, last_sequence)) continue;
        if (filtered && !in_range(r.boot, (uint32_t)(now - r.uptime_ms), current, youngest_ms, oldest_ms)) continue;
        ESP_LOGI(TAG, "Event #%" PRIu32 " boot=%08" PRIx32 " at=%" PRIu32 "ms slot=%u idle=%" PRIu32
            "ms %s detail=%" PRIu32 " repeats=%" PRIu32,
            r.sequence, r.boot, r.uptime_ms, r.slot, r.idle_ms, kms_diag_event_name(r.event, r.detail), r.detail, r.repeats);
    }
    for (unsigned n = 0; n < KMS_DIAGNOSTICS_SNAPSHOTS; ++n) {
        unsigned i = (stats_start + n) % KMS_DIAGNOSTICS_SNAPSHOTS;
        portENTER_CRITICAL(&diagnostic_lock); kms_diag_stats_t s = storage->snapshots[i]; portEXIT_CRITICAL(&diagnostic_lock);
        if (s.version == STORE_VERSION && (!filtered ||
            (s.uptime_ms <= current->uptime_ms && in_range(s.boot, current->uptime_ms - s.uptime_ms,
                                                         current, youngest_ms, oldest_ms)))) print_stats(&s);
    }
    if (!filtered || !youngest_ms) print_stats(current);
    ESP_LOGI(TAG, "History end");
}
static void process_job(const job_t *job)
{
    if (job->type == DUMP) {
        dump_range(&job->data.stats, job->filtered, job->now, job->youngest_ms, job->oldest_ms); return;
    }
    char key[12]; snprintf(key, sizeof(key), "%c%02u", job->type == WRITE_EVENT ? 'e' : 's', job->index);
    const void *data = job->type == WRITE_EVENT ? (const void *)&job->data.event : (const void *)&job->data.stats;
    size_t size = job->type == WRITE_EVENT ? sizeof(job->data.event) : sizeof(job->data.stats);
    nvs_handle_t h = 0;
    esp_err_t e = ESP_OK;
    // A suppressed test event is acknowledged below so leaving test mode does
    // not flush a fault storm to flash. The RAM history remains available.
    bool persist = KMS_DIAGNOSTICS_PERSIST && kms_test_mode_history_allowed(kms_now());
    if (persist)
        e = storage_ready ? nvs_open_from_partition("kms_diag", "kms_history", NVS_READWRITE, &h) : ESP_FAIL;
    if (persist && e == ESP_OK) {
        e = nvs_set_blob(h, key, data, size);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) storage_error(e);
    if (job->type == WRITE_EVENT) {
        receipt_t result = {.index = job->index, .sequence = job->data.event.sequence,
            .repeats = job->data.event.repeats, .success = e == ESP_OK};
        (void)xQueueSend(receipts, &result, portMAX_DELAY);
    }
}
static void worker(void *arg)
{
    (void)arg; job_t job;
    while (true) if (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) process_job(&job);
}
void kms_diagnostics_init(uint32_t boot)
{
    if (!KMS_DIAGNOSTICS_ENABLED) return;
    boot_id = boot;
    storage = kms_memory_calloc(1, sizeof(*storage));
    if (!storage) {
        ESP_LOGW(TAG, "History memory unavailable; input continues");
        return;
    }
    kms_diag_model_init(&storage->model, kms_now(), boot, (uint32_t)esp_reset_reason());
    storage_ready = !KMS_DIAGNOSTICS_PERSIST || nvs_flash_init_partition("kms_diag") == ESP_OK;
    if (!storage_ready) { ++storage->model.stats.storage_errors; ESP_LOGW(TAG, "History partition unavailable; RAM history continues"); }
    nvs_handle_t h = 0;
    if (KMS_DIAGNOSTICS_PERSIST && storage_ready && nvs_open_from_partition("kms_diag", "kms_history", NVS_READONLY, &h) == ESP_OK) {
        for (unsigned i = 0; i < KMS_DIAGNOSTICS_EVENTS; ++i) {
            kms_diag_record_t r;
            if (read_record(h, "e", i, &r, sizeof(r)) == ESP_OK && r.version == STORE_VERSION) {
                storage->model.records[i] = r; storage->saved_sequence[i] = r.sequence; storage->saved_repeats[i] = r.repeats;
                if (!storage->model.next_sequence || kms_newer(r.sequence, storage->model.next_sequence)) {
                    storage->model.next_sequence = r.sequence; storage->model.next = (i + 1) % KMS_DIAGNOSTICS_EVENTS;
                }
            }
        }
        for (unsigned i = 0; i < KMS_DIAGNOSTICS_SNAPSHOTS; ++i) {
            kms_diag_stats_t s;
            if (read_record(h, "s", i, &s, sizeof(s)) == ESP_OK && s.version == STORE_VERSION) {
                storage->snapshots[i] = s;
                if (!storage->model.stats.sequence || kms_newer(s.sequence, storage->model.stats.sequence)) {
                    storage->model.stats.sequence = s.sequence; stat_index = (i + 1) % KMS_DIAGNOSTICS_SNAPSHOTS;
                }
            }
        }
        nvs_close(h);
    }
    jobs = kms_memory_queue_create(KMS_DIAGNOSTICS_QUEUE_DEPTH, sizeof(job_t));
    receipts = kms_memory_queue_create(KMS_DIAGNOSTICS_QUEUE_DEPTH + 2, sizeof(receipt_t));
    if (!jobs || !receipts || xTaskCreate(worker, "kms-history", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "History worker unavailable; RAM counters remain available");
        if (jobs) kms_memory_queue_delete(jobs);
        if (receipts) kms_memory_queue_delete(receipts);
        jobs = receipts = NULL; ++storage->model.stats.storage_errors;
    }
    initialized = true;
    kms_diagnostics_event(KMS_EVENT_BOOT, 0, storage->model.stats.reset_reason);
}
static bool queue_event(unsigned i)
{
    if (!jobs || storage->pending_sequence[i]) return false;
    job_t job = {.type = WRITE_EVENT, .index = i};
    portENTER_CRITICAL(&diagnostic_lock); job.data.event = storage->model.records[i]; portEXIT_CRITICAL(&diagnostic_lock);
    if (xQueueSend(jobs, &job, 0) != pdTRUE) return false;
    storage->pending_sequence[i] = job.data.event.sequence; storage->last_attempt[i] = kms_now(); return true;
}
static void add_event(enum kms_event e, unsigned slot, uint32_t detail, const kms_diag_record_t *recovered)
{
    if (!initialized) return;
    uint32_t now = kms_now();
    portENTER_CRITICAL(&diagnostic_lock);
    unsigned i = kms_diag_model_event(&storage->model, now, e, slot, detail);
    if (recovered && i < KMS_DIAGNOSTICS_EVENTS) {
        storage->model.records[i].boot = recovered->boot;
        storage->model.records[i].uptime_ms = recovered->uptime_ms;
        storage->model.records[i].idle_ms = recovered->idle_ms;
    }
    portEXIT_CRITICAL(&diagnostic_lock);
    if (i >= KMS_DIAGNOSTICS_EVENTS) return;
    if (e == KMS_EVENT_RECOVERED) storage->model.stats.recovery_ms += detail;
    if (e == KMS_EVENT_REPLAY_DROPPED) storage->model.stats.dropped += detail;
    // The first occurrence is queued immediately; repeated occurrences are
    // checkpointed at most once per repeat interval by tick().
    if (jobs && storage->model.records[i].repeats == 1) {
        if (!queue_event(i)) ++storage->model.stats.storage_errors;
    }
    if (storage->model.records[i].repeats == 1) ESP_LOGW(TAG, "Slot %u: %s (detail=%" PRIu32 ", idle=%" PRIu32 "ms)",
        slot, kms_diag_event_name(e, detail), detail, storage->model.records[i].idle_ms);
}
void kms_diagnostics_event(enum kms_event e, unsigned slot, uint32_t detail)
{ add_event(e, slot, detail, NULL); }
void kms_diagnostics_capture_event(kms_diag_record_t *out, enum kms_event e, unsigned slot, uint32_t detail)
{
    uint32_t now = kms_now();
    *out = (kms_diag_record_t){.version = STORE_VERSION, .boot = storage ? storage->model.stats.boot : boot_id,
        .uptime_ms = now, .idle_ms = storage && !storage->model.held ? now - storage->model.last_input : 0,
        .event = (uint16_t)e, .slot = (uint16_t)slot, .detail = detail, .repeats = 1};
}
void kms_diagnostics_recover_event(const kms_diag_record_t *record)
{ if (record && record->version == STORE_VERSION) add_event(record->event, record->slot, record->detail, record); }
void kms_diagnostics_save(void)
{
    if (!initialized || !jobs) return;
    kms_diag_model_tick(&storage->model, kms_now());
    ++storage->model.stats.sequence;
    job_t job = {.type = WRITE_STATS, .index = stat_index}; job.data.stats = storage->model.stats;
    if (xQueueSend(jobs, &job, 0) == pdTRUE) {
        portENTER_CRITICAL(&diagnostic_lock); storage->snapshots[stat_index] = storage->model.stats; portEXIT_CRITICAL(&diagnostic_lock);
        stat_index = (stat_index + 1) % KMS_DIAGNOSTICS_SNAPSHOTS;
    } else ++storage->model.stats.storage_errors;
}
void kms_diagnostics_tick(void)
{
    if (!initialized) return;
    uint32_t now = kms_now(); kms_diag_model_tick(&storage->model, now);
    receipt_t result;
    while (receipts && xQueueReceive(receipts, &result, 0) == pdTRUE) {
        if (storage->pending_sequence[result.index] == result.sequence) storage->pending_sequence[result.index] = 0;
        if (result.success && storage->model.records[result.index].sequence == result.sequence) {
            storage->saved_sequence[result.index] = result.sequence; storage->saved_repeats[result.index] = result.repeats;
        }
    }
    portENTER_CRITICAL(&diagnostic_lock); uint32_t errors = storage_errors; storage_errors = 0; portEXIT_CRITICAL(&diagnostic_lock);
    storage->model.stats.storage_errors += errors;
    if (kms_diag_model_checkpoint(&storage->model, now)) kms_diagnostics_save();
    // Bound retry work independently of retained history. New records retry promptly;
    // grouped repeats are checkpointed at most once per repeat interval.
    for (unsigned n = 0; jobs && n < KMS_DIAGNOSTICS_SCAN_BATCH; ++n) {
        unsigned i = scan_index; scan_index = (scan_index + 1) % KMS_DIAGNOSTICS_EVENTS;
        if (!storage->model.records[i].version || (storage->saved_sequence[i] == storage->model.records[i].sequence && storage->saved_repeats[i] == storage->model.records[i].repeats) ||
            !kms_time_elapsed(now, storage->last_attempt[i],
                storage->saved_sequence[i] == storage->model.records[i].sequence ? KMS_DIAGNOSTICS_REPEAT_MS : 1000u)) continue;
        storage->last_attempt[i] = now;
        (void)queue_event(i);
    }
}
void kms_diagnostics_input(const kms_state_t *previous, const kms_sample_t *s, bool held)
{
    if (initialized) kms_diag_model_input(&storage->model, kms_now(),
        !kms_state_equal(previous, &s->state) || s->x || s->y || s->wheel || s->pan, held);
}
void kms_diagnostics_radio(bool transmit, bool retry, bool replay)
{
    if (!initialized) return;
    if (transmit) ++storage->model.stats.radio_tx; else ++storage->model.stats.radio_rx;
    if (retry) ++storage->model.stats.retries;
    if (replay && !retry) ++storage->model.stats.replayed;
}
void kms_diagnostics_held(bool held)
{
    if (!initialized) return;
    kms_diag_model_tick(&storage->model, kms_now());
    if (storage->model.held != held) storage->model.last_input = kms_now();
    storage->model.held = held;
}
void kms_diagnostics_buffer(unsigned count)
{ if (initialized && count > storage->model.stats.peak_buffer) storage->model.stats.peak_buffer = count; }
void kms_diagnostics_status(void)
{
    if (!initialized) return;
    const kms_diag_stats_t *s = &storage->model.stats;
    ESP_LOGI(TAG, "History=%u events/%u checkpoints RAM=%u bytes heap-free=%u largest-block=%u",
             KMS_DIAGNOSTICS_EVENTS, KMS_DIAGNOSTICS_SNAPSHOTS,
             (unsigned)(sizeof(storage->model) + sizeof(storage->snapshots) + sizeof(storage->saved_sequence) + sizeof(storage->saved_repeats) +
                        sizeof(storage->pending_sequence) + sizeof(storage->last_attempt) +
                        KMS_DIAGNOSTICS_QUEUE_DEPTH * sizeof(job_t) + (KMS_DIAGNOSTICS_QUEUE_DEPTH + 2) * sizeof(receipt_t)),
             (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Boot=%08" PRIx32 " reset=%" PRIu32 " uptime=%" PRIu64 "ms idle=%" PRIu64
        "ms now-idle=%" PRIu32 "ms timeouts=%" PRIu32 " recovered=%" PRIu32 " dropped=%" PRIu32
        " retries=%" PRIu32 " storage-errors=%" PRIu32,
        s->boot, s->reset_reason, s->uptime_ms, s->idle_ms, storage->model.held ? 0 : kms_now() - storage->model.last_input,
        s->events[KMS_EVENT_ACK_TIMEOUT], s->events[KMS_EVENT_RECOVERED], s->dropped, s->retries, s->storage_errors);
}
static void queue_dump(bool filtered, uint32_t youngest_ms, uint32_t oldest_ms)
{
    if (!initialized || !jobs) return;
    job_t job = {.type = DUMP, .filtered = filtered, .now = kms_now(),
                 .youngest_ms = youngest_ms, .oldest_ms = oldest_ms};
    kms_diag_model_tick(&storage->model, job.now); job.data.stats = storage->model.stats;
    if (xQueueSend(jobs, &job, 0) != pdTRUE) ESP_LOGW(TAG, "History worker busy; retry history command");
}

void kms_diagnostics_dump(void) { queue_dump(false, 0, 0); }
static bool parse_minutes(const char **text, uint32_t *minutes)
{
    const char *p = *text;
    while (*p == ' ') ++p;
    if (*p < '0' || *p > '9') return false;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (value > (UINT32_MAX / 60000u - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *minutes = value; *text = p; return true;
}
bool kms_diagnostics_console(const char *line)
{
    static const char prefix[] = "history";
    for (unsigned n = 0; n < sizeof(prefix) - 1; ++n) if (line[n] != prefix[n]) return false;
    if (line[7] && line[7] != ' ') return false;
    const char *p = line + 7;
    while (*p == ' ') ++p;
    if (!*p) { kms_diagnostics_dump(); return true; }
    uint32_t first, second;
    bool valid = parse_minutes(&p, &first);
    uint32_t youngest = 0, oldest = valid ? first : 0;
    if (valid && *p == ' ') {
        while (*p == ' ') ++p;
        if (*p) {
            valid = parse_minutes(&p, &second);
            if (valid) { youngest = first < second ? first : second; oldest = first > second ? first : second; }
        }
    }
    while (*p == ' ') ++p;
    if (!valid || *p) ESP_LOGW(TAG, "Usage: history [minutes [minutes]]; whole minutes 0..%u",
                             (unsigned)(UINT32_MAX / 60000u));
    else queue_dump(true, youngest * 60000u, oldest * 60000u);
    return true;
}

#else
// Shared link code calls these hooks on both boards. S2 diagnostics use their
// own journal; do not link the S3 retention arrays into the smaller receiver.
void kms_diagnostics_init(uint32_t boot) { (void)boot; }
void kms_diagnostics_tick(void) { }
void kms_diagnostics_event(enum kms_event event, unsigned slot, uint32_t detail)
{ (void)event; (void)slot; (void)detail; }
void kms_diagnostics_capture_event(kms_diag_record_t *out, enum kms_event event, unsigned slot, uint32_t detail)
{ (void)out; (void)event; (void)slot; (void)detail; }
void kms_diagnostics_recover_event(const kms_diag_record_t *record) { (void)record; }
void kms_diagnostics_input(const kms_state_t *previous, const kms_sample_t *sample, bool held)
{ (void)previous; (void)sample; (void)held; }
void kms_diagnostics_held(bool held) { (void)held; }
void kms_diagnostics_radio(bool transmit, bool retry, bool replay)
{ (void)transmit; (void)retry; (void)replay; }
void kms_diagnostics_buffer(unsigned count) { (void)count; }
void kms_diagnostics_status(void) { }
void kms_diagnostics_dump(void) { }
bool kms_diagnostics_console(const char *line) { (void)line; return false; }
void kms_diagnostics_save(void) { }
#endif
