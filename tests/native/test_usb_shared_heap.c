// Execute production lifecycle + unchanged SDK allocators against real TLSF.
// Shared DMA/default pool and non-DMA fallback model S3 allocation priorities.
// Native TLSF uses x64 headers; this is not a byte-exact captured target heap.
#include <stdint.h>
#include <stddef.h>
static unsigned heap_errors, heap_error_line;
static int native_clz(size_t value)
{ unsigned bits = 0; for (uint32_t n = (uint32_t)value; n; n >>= 1) ++bits; return 32 - (int)bits; }
#define __builtin_clz native_clz
static int native_ffs(unsigned value)
{ if (!value) return 0; int bit = 1; while (!(value & 1)) { ++bit; value >>= 1; } return bit; }
#define __builtin_ffs native_ffs
// Select TLSF's GCC bit-scan branch with portable native replacements, avoiding
// a platform CRT/intrinsics dependency. Allocator source bodies stay unchanged.
#ifdef _MSC_VER
#define __GNUC__ 4
#define __GNUC_MINOR__ 0
#define __GNUC_PATCHLEVEL__ 1
#define __attribute__(value)
#endif
#define tlsf_assert(value) do { if (!(value)) { ++heap_errors; heap_error_line = __LINE__; } } while (0)
#ifdef _MSC_VER
#pragma warning(push)
// The unchanged SDK allocator intentionally asserts constant native layout facts.
#pragma warning(disable: 4127)
#endif
#include "sdk_tlsf_native.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#undef __GNUC__
#undef __GNUC_MINOR__
#undef __GNUC_PATCHLEVEL__
#endif
bool tlsf_check_hook(void *start, size_t size, bool is_free)
{ (void)start; (void)size; (void)is_free; return true; }
void block_absorb_post_hook(void *start, size_t size, bool is_free)
{ (void)start; (void)size; (void)is_free; }
#define KMS_USB_SHARED_HEAP
#include "test_usb_host.c"

static __declspec(align(512)) uint8_t shared_pool[49152], fallback_pool[8192];
static __declspec(align(512)) uint8_t psram_pool[32768];
static tlsf_t shared_heap, fallback_heap, psram_heap;
static bool psram_enabled;
static unsigned live_allocations, aligned_failures, matrix_cases, current_case;
static bool pair_active(unsigned address);
void kms_shared_sdk_reset(void);
unsigned kms_shared_sdk_errors(void);
static bool in_psram(const void *pointer)
{
    uintptr_t at = (uintptr_t)pointer;
    return at >= (uintptr_t)psram_pool && at < (uintptr_t)(psram_pool + sizeof(psram_pool));
}
void *heap_caps_malloc(size_t bytes, uint32_t caps)
{
    if (caps != (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) { ++heap_errors; return NULL; }
    if (!psram_enabled) return NULL;
    void *result = tlsf_malloc(psram_heap, bytes);
    if (result) ++live_allocations;
    return result;
}
void *calloc(size_t count, size_t bytes)
{
    if (!count || bytes > (size_t)-1 / count) return NULL;
    void *result = tlsf_malloc(shared_heap, count * bytes);
    if (!result) result = tlsf_malloc(fallback_heap, count * bytes);
    if (result) { memset(result, 0, count * bytes); ++live_allocations; }
    return result;
}
void free(void *pointer)
{
    if (!pointer) return;
    uintptr_t at = (uintptr_t)pointer;
    if (at >= (uintptr_t)shared_pool && at < (uintptr_t)(shared_pool + sizeof(shared_pool))) tlsf_free(shared_heap, pointer);
    else if (at >= (uintptr_t)fallback_pool && at < (uintptr_t)(fallback_pool + sizeof(fallback_pool))) tlsf_free(fallback_heap, pointer);
    else if (in_psram(pointer)) tlsf_free(psram_heap, pointer);
    else { ++heap_errors; return; }
    --live_allocations;
}
void *realloc(void *pointer, size_t bytes)
{
    // CONFIG_SPIRAM_USE_CAPS_ALLOC leaves ordinary realloc internal-only.
    // The SDK migrates an explicit PSRAM allocation back into internal RAM.
    if (in_psram(pointer)) {
        void *result = calloc(1, bytes);
        if (result) { memcpy(result, pointer, bytes); free(pointer); }
        return result;
    }
    uintptr_t at = (uintptr_t)pointer;
    if (at >= (uintptr_t)shared_pool && at < (uintptr_t)(shared_pool + sizeof(shared_pool))) return tlsf_realloc(shared_heap, pointer, bytes);
    return tlsf_realloc(fallback_heap, pointer, bytes);
}
void *heap_caps_realloc(void *pointer, size_t bytes, uint32_t caps)
{
    if (caps != MALLOC_CAP_8BIT) { ++heap_errors; return NULL; }
    if (in_psram(pointer)) return tlsf_realloc(psram_heap, pointer, bytes);
    return realloc(pointer, bytes);
}
static void announce_combo(unsigned address, bool vendor_interface)
{
    static const uint8_t vendor_report[] = {6,0,255,9,1,0xa1,1,0x15,0,0x26,255,0,0x75,8,0x95,1,0x81,2,0xc0};
    announce(address, 1, 3, vendor_interface ? 3 : 2);
    if (!vendor_interface) return;
    fake_device_t *d = &fake_devices[address];
    d->reports[2] = vendor_report; d->report_bytes[2] = sizeof(vendor_report);
    unsigned number = 0;
    for (unsigned at = sizeof(usb_config_desc_t); at < ((usb_config_desc_t *)d->config)->wTotalLength; at += d->config[at]) {
        if (d->config[at + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) number = d->config[at + 2];
        if (d->config[at + 1] == HID_DESCRIPTOR && number == 2) d->config[at + 7] = sizeof(vendor_report);
    }
}
static int psram_parser_case(unsigned bytes, bool reverse)
{
    ++matrix_cases; current_case = 0x10000u | bytes | (unsigned)reverse << 17;
    shared_heap = tlsf_create_with_pool(shared_pool, bytes, bytes);
    fallback_heap = tlsf_create_with_pool(fallback_pool, sizeof(fallback_pool), sizeof(fallback_pool));
    psram_heap = tlsf_create_with_pool(psram_pool, sizeof(psram_pool), sizeof(psram_pool));
    CHECK(shared_heap && fallback_heap && psram_heap);
    live_allocations = aligned_failures = heap_errors = 0; psram_enabled = true;
    kms_shared_sdk_reset(); reset(true);
    announce(1, 0, 9, 1); announce_combo(2, reverse); pump(30);
    CHECK(pair_active(2));
    announce_combo(3, !reverse); pump(30);
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        CHECK(pair_active(2) && pair_active(3));
        for (unsigned n = 0; n < KMS_USB_INTERFACES; ++n) if (interfaces[n].active) {
            CHECK(in_psram(interfaces[n].parser));
            CHECK(tlsf_block_size(interfaces[n].parser) < sizeof(kms_hid_parser_t));
        }
        unplug(3); pump(15); CHECK(pair_active(2));
        announce_combo(3, !reverse); pump(30);
    }
    CHECK(pair_active(2) && pair_active(3));
    unplug(3); unplug(2); unplug(1); pump(20);
    CHECK(!live_allocations && !heap_errors && !aligned_failures && claims == releases);
    CHECK(tlsf_check(shared_heap) == 0 && tlsf_check(fallback_heap) == 0 && tlsf_check(psram_heap) == 0);
    psram_enabled = false;
    return 0;
}
void *kms_shared_aligned(size_t align, size_t bytes)
{
    // The real esp_heap_adjust_alignment_to_hw rounds size to requested DMA
    // alignment, not merely cache alignment. A 256-byte list consumes 512.
    bytes = (bytes + align - 1) & ~(align - 1);
    void *result = tlsf_memalign(shared_heap, align, bytes);
    if (result) { memset(result, 0, bytes); ++live_allocations; }
    else ++aligned_failures;
    return result;
}
int printf(const char *format, ...) { (void)format; return 0; }
static bool pair_active(unsigned address)
{
    kms_usb_inventory_t inventory; kms_usb_devices_snapshot(&inventory);
    const kms_usb_interface_info_t *a = row(&inventory, address, 0), *b = row(&inventory, address, 1);
    return a && b && a->state == KMS_USB_ACTIVE && b->state == KMS_USB_ACTIVE;
}
static int retained_parser_case(void)
{
    const uint8_t descriptor[] = {5,1,9,6,0xa1,1,5,7,0x19,0,0x29,0x65,0x15,0,0x25,0x65,
        0x75,8,0x95,1,0x81,0,5,8,0x19,1,0x29,5,0x15,0,0x25,1,0x75,1,0x95,5,0x91,2,
        0x75,3,0x95,1,0x91,1,0xc0};
    kms_hid_parser_t *parser = calloc(1, sizeof(*parser)); CHECK(parser != NULL);
    CHECK(kms_hid_parse(parser, descriptor, sizeof(descriptor)) && parser->count && parser->led_count);
    uint8_t count = parser->count; size_t bytes = kms_hid_parser_storage_size(parser);
    CHECK(bytes < sizeof(*parser));
    parser->count = KMS_HID_FIELDS; CHECK(kms_hid_parser_storage_size(parser) == sizeof(*parser)); parser->count = count;
    parser = realloc(parser, bytes); CHECK(parser != NULL);
    // Reuse released storage before decoding/LED encoding, so reads of the
    // discarded input-field tail cannot accidentally rely on old contents.
    void *reuse = calloc(1, 8192); CHECK(reuse != NULL); memset(reuse, 0xa5, 8192);
    kms_sample_t sample; uint8_t report = 4, leds[2] = {0, 0xa5};
    CHECK(kms_hid_decode(parser, &report, 1, &sample) && kms_key(&sample.state, 4));
    CHECK(kms_hid_led_report_supported(parser, 0));
    CHECK(kms_hid_led_report(parser, 0, 5, leds, sizeof(leds)) == 1 && leds[0] == 5 && leds[1] == 0xa5);
    free(reuse); free(parser); CHECK(!live_allocations);
    return 0;
}
__declspec(dllexport) unsigned kms_usb_shared_heap_checks(void) { return checks; }
__declspec(dllexport) unsigned kms_usb_shared_heap_cases(void) { return matrix_cases; }
__declspec(dllexport) unsigned kms_usb_shared_heap_case(void) { return current_case; }
__declspec(dllexport) unsigned kms_usb_shared_heap_error_line(void) { return heap_error_line; }
__declspec(dllexport) int kms_run_usb_shared_heap_tests(void)
{
    checks = matrix_cases = 0;
    for (unsigned bytes = 24576; bytes <= 49152; bytes += 512) {
        ++matrix_cases; current_case = bytes;
        shared_heap = tlsf_create_with_pool(shared_pool, bytes, bytes);
        fallback_heap = tlsf_create_with_pool(fallback_pool, sizeof(fallback_pool), sizeof(fallback_pool));
        CHECK(shared_heap && fallback_heap);
        live_allocations = aligned_failures = heap_errors = 0;
        int result = retained_parser_case(); if (result) return result;
        kms_shared_sdk_reset(); reset(true);
        announce(1, 0, 9, 1); announce(2, 1, 3, 2); pump(30);
        CHECK(pair_active(2));
        announce(3, 1, 3, 2); pump(30);
        CHECK(pair_active(2) && pair_active(3));
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
            unplug(3); pump(15); CHECK(pair_active(2));
            announce(3, 1, 3, 2); pump(30);
            CHECK(pair_active(2) && pair_active(3));
            CHECK(!aligned_failures && !heap_errors && !kms_shared_sdk_errors() && !mistakes);
            CHECK(tlsf_check(shared_heap) == 0 && tlsf_check(fallback_heap) == 0);
        }
        unplug(3); unplug(2); unplug(1); pump(20);
        CHECK(!live_allocations && !heap_errors && claims == releases);
        CHECK(tlsf_check(shared_heap) == 0 && tlsf_check(fallback_heap) == 0);
    }
    for (unsigned bytes = 16384; bytes <= sizeof(shared_pool); bytes += 512)
        for (unsigned reverse = 0; reverse < 2; ++reverse) {
            int result = psram_parser_case(bytes, reverse != 0); if (result) return result;
        }
    return 0;
}
