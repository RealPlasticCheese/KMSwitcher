// Production allocation policy with deterministic internal/external heaps.
#include "../../firmware/components/kms_runtime/memory.c"
#include <string.h>
#include <stdbool.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, mistakes, ordinary_calls, external_calls, caps_calls, deletes;
static bool external_failed, internal_failed;
static uint8_t internal_bytes[256], external_bytes[256];
static unsigned external_queue, internal_queue, ordinary_queue;
void *calloc(size_t count, size_t bytes)
{
    ++ordinary_calls;
    if (internal_failed || !count || bytes > sizeof(internal_bytes) / count) return NULL;
    memset(internal_bytes, 0, count * bytes); return internal_bytes;
}
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
void *heap_caps_calloc(size_t count, size_t bytes, uint32_t caps)
{
    ++external_calls;
    if (caps != (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) ++mistakes;
    if (external_failed || !count || bytes > sizeof(external_bytes) / count) return NULL;
    memset(external_bytes, 0, count * bytes); return external_bytes;
}
QueueHandle_t xQueueCreateWithCaps(unsigned count, unsigned bytes, unsigned caps)
{
    ++caps_calls;
    if (count != 8 || bytes != 32) ++mistakes;
    if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return external_failed ? NULL : &external_queue;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return internal_failed ? NULL : &internal_queue;
    ++mistakes; return NULL;
}
void vQueueDeleteWithCaps(QueueHandle_t queue)
{ if (queue != &external_queue && queue != &internal_queue) ++mistakes; ++deletes; }
#endif
QueueHandle_t xQueueCreate(unsigned count, unsigned bytes)
{ if (count != 8 || bytes != 32) ++mistakes; return internal_failed ? NULL : &ordinary_queue; }
void vQueueDelete(QueueHandle_t queue)
{ if (queue != &ordinary_queue) ++mistakes; ++deletes; }
__declspec(dllexport) unsigned kms_memory_checks(void) { return checks; }
__declspec(dllexport) int kms_run_memory_tests(void)
{
    CHECK(!kms_memory_calloc(0, 16) && !kms_memory_calloc(2, 0));
    CHECK(!kms_memory_calloc(SIZE_MAX, 2) && !ordinary_calls && !external_calls);
    memset(internal_bytes, 0xa5, sizeof(internal_bytes)); memset(external_bytes, 0xa5, sizeof(external_bytes));
    uint8_t *buffer = kms_memory_calloc(2, 32);
    CHECK(buffer != NULL);
    for (unsigned n = 0; n < 64; ++n) CHECK(buffer[n] == 0);
    QueueHandle_t queue = kms_memory_queue_create(8, 32);
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    CHECK(buffer == external_bytes && external_calls == 1 && !ordinary_calls);
    CHECK(queue == &external_queue && caps_calls == 1);
#else
    CHECK(buffer == internal_bytes && !external_calls && ordinary_calls == 1);
    CHECK(queue == &ordinary_queue && !caps_calls);
#endif
    kms_memory_queue_delete(queue); CHECK(deletes == 1);
    external_failed = true;
    buffer = kms_memory_calloc(4, 32); CHECK(buffer == internal_bytes);
    queue = kms_memory_queue_create(8, 32);
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    CHECK(queue == &internal_queue && caps_calls == 3);
#else
    CHECK(queue == &ordinary_queue && !caps_calls);
#endif
    kms_memory_queue_delete(queue); CHECK(deletes == 2);
    internal_failed = true;
    CHECK(!kms_memory_calloc(4, 32) && !kms_memory_queue_create(8, 32));
    kms_memory_queue_delete(NULL); CHECK(deletes == 2 && !mistakes);
    return 0;
}
