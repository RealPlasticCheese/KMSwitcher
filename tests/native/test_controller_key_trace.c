// Production logger, bounded storage and worker step; fake RTOS and UART only.
#include <stdarg.h>
#include <stdint.h>
static int trace_strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
#define strcmp trace_strcmp
static void trace_log(unsigned level, const char *tag, const char *format, ...);
#define ESP_LOG_WARN 2
#define ESP_LOG_INFO 3
#define ESP_LOG_LEVEL_LOCAL(level, tag, ...) trace_log(level, tag, __VA_ARGS__)
#include "key_trace.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)

static key_record_t storage[TRACE_RECORDS], queued[FAILURE_QUEUE];
static unsigned head, queued_count, checks, mistakes, warnings, infos, notices, notifications;
static unsigned last_page, last_usage, last_slot;
static const char *last_reason, *last_stage, *last_direction;
static bool psram_available, task_available, queue_available;
static uint32_t clock_ms;
uint32_t kms_now(void) { return clock_ms; }
unsigned kms_link_selected(void) { return 3; }
void *heap_caps_calloc(size_t count, size_t bytes, uint32_t caps)
{
    if (count != TRACE_RECORDS || bytes != sizeof(key_record_t) || caps != (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) ++mistakes;
    memset(storage, 0, sizeof(storage)); return psram_available ? storage : NULL;
}
QueueHandle_t xQueueCreate(unsigned count, unsigned bytes)
{
    if ((count != FAILURE_QUEUE && count != 64) || bytes != sizeof(key_record_t)) ++mistakes;
    head = queued_count = 0; return queue_available ? queued : NULL;
}
QueueHandle_t xQueueCreateWithCaps(unsigned count, unsigned bytes, unsigned caps)
{
    if (caps != (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) ++mistakes;
    return psram_available ? xQueueCreate(count, bytes) : NULL;
}
int xQueueSend(QueueHandle_t queue, const void *item, uint32_t wait)
{
    if (queue != queued || wait) ++mistakes;
    if (queued_count == FAILURE_QUEUE) return pdFALSE;
    queued[(head + queued_count++) % FAILURE_QUEUE] = *(const key_record_t *)item; return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *item, uint32_t wait)
{
    if (queue != queued || wait) ++mistakes;
    if (!queued_count) return pdFALSE;
    *(key_record_t *)item = queued[head++ % FAILURE_QUEUE]; --queued_count; return pdTRUE;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{
    (void)name; (void)arg;
    if (fn != trace_worker || stack < 3072 || priority != 1) ++mistakes;
    *(TaskHandle_t *)handle = task_available ? (void *)1 : NULL;
    return task_available ? pdPASS : pdFALSE;
}
void xTaskNotifyGive(TaskHandle_t handle) { if (!handle) ++mistakes; ++notifications; }
unsigned ulTaskNotifyTake(int clear, unsigned ticks) { (void)clear; (void)ticks; return 0; }
void vTaskDelay(unsigned ticks) { (void)ticks; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; ++notices; }
static void trace_log(unsigned level, const char *tag, const char *format, ...)
{
    (void)tag; (void)format;
    if (level == ESP_LOG_WARN) ++warnings; else ++infos;
    va_list args; va_start(args, format);
    last_stage = va_arg(args, const char *);
    (void)va_arg(args, uint32_t); last_slot = va_arg(args, int);
    (void)va_arg(args, int); // Source.
    for (unsigned n = 0; n < 3; ++n) (void)va_arg(args, uint32_t);
    last_page = va_arg(args, int); last_usage = va_arg(args, int);
    last_direction = va_arg(args, const char *); last_reason = va_arg(args, const char *);
    va_end(args);
}
static void setup(void)
{
    next_record = overwritten = lost_warnings = 0; records = NULL; failures = NULL; worker = NULL;
    dump_requested = false; warnings = infos = notices = notifications = 0;
    psram_available = task_available = queue_available = true;
    kms_controller_key_trace_init();
}
static void event(enum kms_key_trace_stage stage, enum kms_key_trace_reason reason, bool down)
{
    kms_state_t zero = {0}, key = {0}; kms_set_key(&key, 4, true);
    kms_key_trace_emit(kms_controller_key_trace, NULL, stage, reason, 1, 2, ++clock_ms, 5, 7,
        down ? &zero : &key, down ? &key : &zero);
}
__declspec(dllexport) int kms_run_controller_key_trace_tests(void)
{
    checks = mistakes = 0; setup();
    CHECK(records && failures && worker && !notices);
    event(KMS_KEY_QUEUED, KMS_KEY_OK, true); event(KMS_KEY_CONFIRMED, KMS_KEY_OK, true);
    CHECK(!warnings && !infos && next_record == 2); // Success never floods UART.
    event(KMS_KEY_MISSING, KMS_KEY_USB_QUEUE_FULL, false);
    CHECK(!warnings && queued_count == 1 && notifications == 1);
    trace_tick();
    CHECK(warnings == 1 && last_page == 7 && last_usage == 4 && last_slot == 2);
    CHECK(!strcmp(last_stage, "missing") && !strcmp(last_direction, "up"));
    CHECK(!strcmp(last_reason, "usb-report-queue-full"));
    event(KMS_KEY_UNCONFIRMED, KMS_KEY_RECEIVER_RESTART, true); trace_tick();
    CHECK(warnings == 2 && !strcmp(last_stage, "unconfirmed"));
    CHECK(!strcmp(last_reason, "receiver-restarted-receipt-unknown"));
    kms_state_t zero = {0}, key = {0}; kms_set_key(&key, 6, true);
    kms_key_trace_emit(kms_controller_router_key_trace, NULL, KMS_KEY_MISSING,
        KMS_KEY_SOURCE_REMOVED, 1, 0, 0, 0, 0, &zero, &key);
    trace_tick(); CHECK(last_slot == 3 && last_usage == 6);
    for (unsigned n = 0; n < TRACE_RECORDS * 2; ++n) event(KMS_KEY_CONFIRMED, KMS_KEY_OK, n % 2 != 0);
    CHECK(overwritten == next_record - TRACE_RECORDS);
    CHECK(kms_controller_key_trace_console("key-trace") && dump_requested);
    trace_tick(); CHECK(infos == TRACE_RECORDS && !dump_requested);
    CHECK(!kms_controller_key_trace_console("key-trace invalid"));
    for (unsigned n = 0; n < FAILURE_QUEUE + 3; ++n) event(KMS_KEY_MISSING, KMS_KEY_BUFFER_FULL, true);
    CHECK(queued_count == FAILURE_QUEUE && lost_warnings == 3);
    unsigned previous_notices = notices;
    trace_tick(); CHECK(!queued_count && !lost_warnings && notices == previous_notices + 1);
    setup(); psram_available = false; kms_controller_key_trace_init();
    CHECK(!records && worker && notices == 1);
    event(KMS_KEY_MISSING, KMS_KEY_NO_ROUTE, true); trace_tick(); CHECK(warnings == 1);
    setup(); task_available = false; worker = NULL; kms_controller_key_trace_init();
    event(KMS_KEY_MISSING, KMS_KEY_NO_ROUTE, true);
    CHECK(!worker && warnings == 1 && !strcmp(last_reason, "no-selected-dongle"));
    setup(); queue_available = false; worker = NULL; kms_controller_key_trace_init();
    event(KMS_KEY_MISSING, KMS_KEY_NO_ROUTE, true); CHECK(!worker && warnings == 1);
    CHECK(mistakes == 0);
    return 0;
}
__declspec(dllexport) unsigned kms_controller_key_trace_test_count(void) { return checks; }
