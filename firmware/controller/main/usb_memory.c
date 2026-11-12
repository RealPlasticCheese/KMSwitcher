#include "usb_memory.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static DRAM_ATTR portMUX_TYPE memory_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR kms_usb_allocation_t latest;
static kms_usb_memory_failure_t first, last;
static uint32_t failures;

static void IRAM_ATTR allocation_failed(size_t bytes, uint32_t caps, const char *function)
{
    // This callback can also run in an ISR or with caches disabled. Retain only
    // fixed metadata; do not allocate, query heaps, log, or touch USB here.
    uint32_t at = (uint32_t)(esp_timer_get_time() / 1000);
    void *task = xPortInIsrContext() ? NULL : xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL_SAFE(&memory_lock);
    ++latest.sequence; latest.at = at; latest.bytes = bytes;
    latest.caps = caps; latest.function = function;
    latest.task = task;
    portEXIT_CRITICAL_SAFE(&memory_lock);
}

void kms_usb_memory_init(void)
{
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(allocation_failed));
}

static void heap_state(size_t *free_bytes, size_t *largest, size_t *dma_free, size_t *dma_largest)
{
    *free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    *largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    *dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    *dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

uint32_t kms_usb_memory_sequence(void)
{
    portENTER_CRITICAL_SAFE(&memory_lock); uint32_t result = latest.sequence; portEXIT_CRITICAL_SAFE(&memory_lock);
    return result;
}

void kms_usb_memory_failed(unsigned address, unsigned interface_number, uint32_t before)
{
    kms_usb_memory_failure_t record = {.address = address, .interface_number = interface_number,
        .at = (uint32_t)(esp_timer_get_time() / 1000)};
    portENTER_CRITICAL_SAFE(&memory_lock); record.failed = latest; portEXIT_CRITICAL_SAFE(&memory_lock);
    record.matched = record.failed.sequence != before && record.failed.task == xTaskGetCurrentTaskHandle();
    heap_state(&record.free_bytes, &record.largest, &record.dma_free, &record.dma_largest);
    if (!failures++) first = record;
    last = record;
}

void kms_usb_memory_snapshot(kms_usb_memory_status_t *out)
{
    portENTER_CRITICAL_SAFE(&memory_lock); out->latest = latest; portEXIT_CRITICAL_SAFE(&memory_lock);
    out->first = first; out->last = last; out->failures = failures;
    heap_state(&out->free_bytes, &out->largest, &out->dma_free, &out->dma_largest);
}

static void print_failure(const char *kind, const kms_usb_memory_failure_t *r)
{
    printf("KMSUSBMEM %s at=%" PRIu32 " target=%u/%u seq=%" PRIu32 " failed-at=%" PRIu32
           " matched=%u bytes=%u caps=%08" PRIx32 " function=%s free=%u largest=%u dma-free=%u dma-largest=%u\n",
        kind, r->at, r->address, r->interface_number, r->failed.sequence, r->failed.at,
        r->matched, (unsigned)r->failed.bytes, r->failed.caps, r->failed.function ? r->failed.function : "none",
        (unsigned)r->free_bytes, (unsigned)r->largest, (unsigned)r->dma_free, (unsigned)r->dma_largest);
}

void kms_usb_memory_print(const kms_usb_memory_status_t *s)
{
    printf("KMSUSBMEM STATE failures=%" PRIu32 " allocator-failures=%" PRIu32
           " free=%u largest=%u dma-free=%u dma-largest=%u\n", s->failures, s->latest.sequence,
        (unsigned)s->free_bytes, (unsigned)s->largest, (unsigned)s->dma_free, (unsigned)s->dma_largest);
    if (s->failures) { print_failure("FIRST", &s->first); print_failure("LAST", &s->last); }
}
