// Production allocation hook: no callback I/O, correlation, frozen first/last evidence.
#include <stdbool.h>
#include "esp_log.h"
static void *xTaskGetCurrentTaskHandle(void);
static int xPortInIsrContext(void);
#include "usb_memory.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, mistakes, queries, prints;
static uint32_t now;
static int task, other_task;
static void *current_task = &task;
static bool isr;
static esp_alloc_failed_hook_t hook;
static kms_usb_memory_status_t saved, after;
static size_t total = 30012, largest_block = 17408, dma = 1048, dma_block = 256;
static void *xTaskGetCurrentTaskHandle(void) { return current_task; }
static int xPortInIsrContext(void) { return isr; }
int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
void kms_fake_check(int result) { if (result != ESP_OK) ++mistakes; }
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback) { hook = callback; return ESP_OK; }
size_t heap_caps_get_free_size(uint32_t caps)
{ ++queries; return caps == MALLOC_CAP_8BIT ? total : caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) ? dma : 0; }
size_t heap_caps_get_largest_free_block(uint32_t caps)
{ ++queries; return caps == MALLOC_CAP_8BIT ? largest_block : caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) ? dma_block : 0; }
int printf(const char *format, ...) { (void)format; ++prints; return 0; }
__declspec(dllexport) unsigned kms_usb_memory_checks(void) { return checks; }
__declspec(dllexport) int kms_run_usb_memory_tests(void)
{
    kms_usb_memory_init(); CHECK(hook != NULL && !mistakes);
    kms_usb_memory_snapshot(&saved); CHECK(!saved.failures && !saved.latest.sequence);
    CHECK(saved.free_bytes == total && saved.dma_largest == dma_block);
    unsigned before_queries = queries;
    now = 416603;
    uint32_t baseline = kms_usb_memory_sequence();
    hook(256, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL, "heap_caps_aligned_alloc");
    CHECK(queries == before_queries && !prints); // No heap query or UART in hook.
    CHECK(kms_usb_memory_sequence() == baseline + 1);
    kms_usb_memory_failed(8, 1, baseline); kms_usb_memory_snapshot(&saved);
    CHECK(saved.failures == 1 && saved.first.matched && saved.last.matched);
    CHECK(saved.first.failed.bytes == 256 && saved.first.failed.at == now && saved.first.at == now);
    CHECK(saved.first.address == 8 && saved.first.interface_number == 1);
    CHECK(saved.first.free_bytes == 30012 && saved.first.dma_largest == 256);
    now += 1000; dma = 512; dma_block = 128;
    baseline = kms_usb_memory_sequence(); hook(512, MALLOC_CAP_DMA, "heap_caps_aligned_alloc");
    kms_usb_memory_failed(8, 1, baseline); kms_usb_memory_snapshot(&after);
    CHECK(after.failures == 2 && after.first.failed.bytes == 256 && after.last.failed.bytes == 512);
    CHECK(after.first.dma_largest == 256 && after.last.dma_largest == 128);
    CHECK(saved.failures == 1 && saved.last.failed.bytes == 256 && saved.dma_largest == 256);
    // An SDK-generated NO_MEM without a new hook must not blame an older failure.
    baseline = kms_usb_memory_sequence(); kms_usb_memory_failed(9, 0, baseline);
    kms_usb_memory_snapshot(&after); CHECK(!after.last.matched);
    // Another task's allocation and ISR allocations must not be attributed to USB.
    baseline = kms_usb_memory_sequence(); current_task = &other_task;
    hook(9000, MALLOC_CAP_8BIT, "heap_caps_malloc"); current_task = &task;
    kms_usb_memory_failed(8, 1, baseline); kms_usb_memory_snapshot(&after); CHECK(!after.last.matched);
    baseline = kms_usb_memory_sequence(); isr = true;
    hook(32, MALLOC_CAP_DMA, "heap_caps_malloc"); isr = false;
    kms_usb_memory_failed(8, 1, baseline); kms_usb_memory_snapshot(&after); CHECK(!after.last.matched);
    // Sequence wrap still correlates and repeats never overwrite the first fault.
    latest.sequence = UINT32_MAX; baseline = kms_usb_memory_sequence(); now = 4;
    hook(24, MALLOC_CAP_DMA, "heap_caps_aligned_alloc");
    kms_usb_memory_failed(10, 0, baseline); kms_usb_memory_snapshot(&after);
    CHECK(after.last.matched && after.last.failed.sequence == 0 && after.last.at == 4);
    CHECK(after.first.failed.at == 416603 && after.first.failed.bytes == 256);
    kms_usb_memory_print(&after); CHECK(prints == 3 && !mistakes);
    return 0;
}
