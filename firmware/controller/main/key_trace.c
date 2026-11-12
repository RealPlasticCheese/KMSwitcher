#include "key_trace.h"
#include "kms_runtime.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>

enum { TRACE_RECORDS = 512, FAILURE_QUEUE = 1024 };
typedef struct {
    uint32_t record, at, id, generation, sequence;
    uint16_t usage;
    uint8_t page, source, slot, stage, reason, down;
} key_record_t;
static key_record_t *records;
static uint32_t next_record, overwritten, lost_warnings;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t failures;
static TaskHandle_t worker;
static bool dump_requested;
static const char *TAG = "key-trace";

static void print_record(const key_record_t *r, bool warning)
{
    ESP_LOG_LEVEL_LOCAL(warning ? ESP_LOG_WARN : ESP_LOG_INFO, TAG,
        "key %s t=%" PRIu32 " dst=%u src=%u id=%" PRIu32 " gen=%" PRIu32
        " seq=%" PRIu32 " page=%02x usage=%04x %s reason=%s",
        kms_key_trace_stage_name(r->stage), r->at, r->slot, r->source,
        r->id, r->generation, r->sequence, r->page, r->usage, r->down ? "down" : "up",
        kms_key_trace_reason_name(r->reason));
}
static void record_change(void *ctx, unsigned page, unsigned usage, bool down)
{
    const kms_key_trace_t *trace = ctx;
    key_record_t record = {.at = kms_now(), .id = trace->id, .generation = trace->generation,
        .sequence = trace->sequence, .usage = (uint16_t)usage, .page = (uint8_t)page,
        .source = (uint8_t)trace->source, .slot = (uint8_t)trace->slot,
        .stage = (uint8_t)trace->stage, .reason = (uint8_t)trace->reason, .down = down};
    portENTER_CRITICAL(&lock);
    record.record = next_record++;
    if (records) {
        records[record.record % TRACE_RECORDS] = record;
        if (next_record > TRACE_RECORDS) ++overwritten;
    }
    portEXIT_CRITICAL(&lock);
    if (trace->stage != KMS_KEY_MISSING && trace->stage != KMS_KEY_UNCONFIRMED) return;
    if (!worker) { print_record(&record, true); return; }
    if (!failures || xQueueSend(failures, &record, 0) != pdTRUE) {
        portENTER_CRITICAL(&lock); ++lost_warnings; portEXIT_CRITICAL(&lock);
    }
    if (worker) xTaskNotifyGive(worker);
}
void kms_controller_key_trace(void *ctx, const kms_key_trace_t *trace)
{
    (void)ctx;
    kms_key_changes(trace->before, trace->after, record_change, (void *)trace);
}
void kms_controller_router_key_trace(void *ctx, const kms_key_trace_t *trace)
{
    // Router events run on the application task and have no destination field.
    kms_key_trace_t event = *trace; event.slot = kms_link_selected();
    kms_controller_key_trace(ctx, &event);
}
void kms_controller_key_trace_drop(enum kms_key_trace_reason reason, unsigned source,
    unsigned slot, const kms_state_t *before, const kms_state_t *after)
{
    kms_key_trace_emit(kms_controller_key_trace, NULL, KMS_KEY_MISSING, reason,
        source, slot, 0, 0, 0, before, after);
}
static void dump(void)
{
    portENTER_CRITICAL(&lock);
    uint32_t end = next_record, replaced = overwritten;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "KEYTRACE BEGIN retained=%" PRIu32 " overwritten=%" PRIu32 " (RAM; confirmed=dongle accepted)",
        records ? (end < TRACE_RECORDS ? end : TRACE_RECORDS) : 0, replaced);
    if (records) for (uint32_t cursor = end > TRACE_RECORDS ? end - TRACE_RECORDS : 0; cursor != end; ++cursor) {
        portENTER_CRITICAL(&lock);
        key_record_t record = records[cursor % TRACE_RECORDS];
        portEXIT_CRITICAL(&lock);
        if (record.record == cursor) print_record(&record, false);
        else ESP_LOGW(TAG, "Trace record overwritten while exporting; capture key-trace again");
        if (!(cursor % 16)) vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGI(TAG, "KEYTRACE END");
}
static void trace_tick(void)
{
    key_record_t record;
    while (xQueueReceive(failures, &record, 0) == pdTRUE) print_record(&record, true);
    portENTER_CRITICAL(&lock);
    uint32_t lost = lost_warnings; lost_warnings = 0;
    bool requested = dump_requested; dump_requested = false;
    portEXIT_CRITICAL(&lock);
    if (lost) ESP_LOGW(TAG, "Key delivery audit incomplete: %" PRIu32 " failure warnings lost (trace output queue full)", lost);
    if (requested) dump();
}
static void trace_worker(void *arg)
{
    (void)arg;
    while (true) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)); trace_tick(); }
}
void kms_controller_key_trace_init(void)
{
    records = heap_caps_calloc(TRACE_RECORDS, sizeof(*records), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    failures = xQueueCreateWithCaps(FAILURE_QUEUE, sizeof(key_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!failures) failures = xQueueCreate(64, sizeof(key_record_t));
    if (!failures || xTaskCreate(trace_worker, "key-trace", 4096, NULL, 1, &worker) != pdPASS) {
        ESP_LOGE(TAG, "Key delivery audit unavailable: trace worker allocation failed");
        return;
    }
    if (!records) ESP_LOGW(TAG, "Key trace RAM allocation failed; missing-key warnings remain enabled");
}
bool kms_controller_key_trace_console(const char *line)
{
    if (strcmp(line, "key-trace")) return false;
    if (!worker) ESP_LOGW(TAG, "Key delivery audit unavailable: trace worker allocation failed");
    else {
        portENTER_CRITICAL(&lock); dump_requested = true; portEXIT_CRITICAL(&lock);
        xTaskNotifyGive(worker);
    }
    return true;
}
