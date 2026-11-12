// Execute selected unmodified SDK allocation functions with deterministic heaps.
// The native port/pipe adapters omit unrelated scheduler fields; this is not an
// HCD scheduler, DMA, ABI or FreeRTOS emulator. See the generated source manifest.
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "usb/usb_host.h"
#include "sdk_allocation_constants.inc"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
#define ESP_ERR_INVALID_ARG 10
#define ESP_ERR_INVALID_STATE 11
#define USB_TRANSFER_TYPE_CTRL 0
#define USB_TRANSFER_TYPE_ISOCHRONOUS 1
#define USB_TRANSFER_TYPE_BULK 2
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_CACHE_ALIGNED 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DEFAULT 8
#define XFER_DESC_LIST_CAPS (MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_INTERNAL)
#define NUM_BUFFERS 2
#define ALIGN_UP(n, a) ((a) == 0 ? (n) : (((n) + (a) - 1) & ~((size_t)(a) - 1)))
#define HCD_ENTER_CRITICAL() ((void)0)
#define HCD_EXIT_CRITICAL() ((void)0)
#define HCD_CHECK(c, e) do { if (!(c)) return (e); } while (0)
#define HCD_CHECK_FROM_CRIT(c, e) HCD_CHECK(c, e)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(e) do { if ((e) != ESP_OK) ++mistakes; } while (0)
#define CACHE_SYNC_FRAME_LIST(p) ((void)(p))
#define HCD_PIPE_STATE_ACTIVE 1
#define TAILQ_INIT(q) ((q)->first = NULL)
#define TAILQ_INSERT_TAIL(q, p, entry) do { (p)->entry.next = (q)->first; (q)->first = (p); } while (0)
#define TAILQ_REMOVE(q, p, entry) do { pipe_t **at = &(q)->first; while (*at && *at != (p)) at = &(*at)->entry.next; if (*at) *at = (p)->entry.next; else ++mistakes; } while (0)
typedef int usb_transfer_type_t;
typedef int usb_speed_t;
typedef struct { unsigned channel; } usb_dwc_hal_chan_t;
typedef struct { unsigned occupied; } usb_dwc_hal_context_t;
typedef struct { unsigned mps; } usb_dwc_hal_ep_char_t;
typedef struct { void *xfer_desc_list; int xfer_desc_list_len_bytes; } dma_buffer_block_t;
typedef struct pipe_obj pipe_t;
typedef struct port_obj {
    usb_dwc_hal_context_t *hal;
    unsigned speed, periph_idx;
    struct { bool conn_dev_ena; } flags;
    struct { pipe_t *first; } pipes_idle_tailq;
    int num_pipes_idle, num_pipes_queued;
    void *frame_list;
} port_t;
struct pipe_obj {
    struct { void *first; } pending_urb_tailq, done_urb_tailq;
    dma_buffer_block_t *buffers[NUM_BUFFERS];
    struct { unsigned buffer_num_to_fill; bool buffer_is_executing; } multi_buffer_control;
    struct { bool has_urb; } cs_flags;
    struct { pipe_t *next; } tailq_entry;
    port_t *port;
    usb_dwc_hal_chan_t *chan_obj;
    usb_dwc_hal_ep_char_t ep_char;
    int state;
    void *callback, *callback_arg, *context;
};
typedef port_t *hcd_port_handle_t;
typedef pipe_t *hcd_pipe_handle_t;
typedef struct {
    void *callback, *callback_arg, *context;
    const usb_ep_desc_t *ep_desc;
    usb_speed_t dev_speed;
    unsigned dev_addr;
} hcd_pipe_config_t;
typedef struct { struct { port_t *port_hdl; usb_speed_t speed; unsigned address; } constant; } device_t;
typedef struct { void *context, *ep_cb, *ep_cb_arg; } usbh_ep_config_t;
typedef struct { struct { pipe_t *pipe_hdl; void *ep_cb, *ep_cb_arg; device_t *dev; const usb_ep_desc_t *ep_desc; } constant; } endpoint_t;
static void *epN_pipe_callback;
static bool s_port_inited[1], compliant, supported;
static unsigned checks, cases, current_case, mistakes, alloc_calls, fail_at, disconnect_at, cache_alignment;
static bool misalign_dma;
static port_t fixture_port;
static usb_dwc_hal_context_t hal;
static struct { uint64_t align; uint8_t bytes[512]; bool used; } heap[128];
static __declspec(align(512)) uint8_t dma_heap[16384];
static uint8_t dma_used[sizeof(dma_heap)];
static struct { void *pointer; unsigned start, bytes; } dma_allocations[32];
static bool allocation_fails(void)
{
    ++alloc_calls;
    if (alloc_calls == disconnect_at) fixture_port.flags.conn_dev_ena = false;
    return alloc_calls == fail_at;
}
#ifdef KMS_USB_SHARED_HEAP
void *calloc(size_t count, size_t bytes);
void free(void *pointer);
void *kms_shared_aligned(size_t align, size_t bytes);
#else
void *calloc(size_t count, size_t bytes)
{
    if (allocation_fails()) return NULL;
    if (!count || bytes > sizeof(heap[0].bytes) / count) { ++mistakes; return NULL; }
    for (unsigned n = 0; n < 128; ++n) if (!heap[n].used) {
        heap[n].used = true; memset(heap[n].bytes, 0, bytes * count); return heap[n].bytes;
    }
    return NULL;
}
#endif
static void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps)
{ if (caps != MALLOC_CAP_DEFAULT) ++mistakes; return calloc(count, bytes); }
static void *heap_caps_aligned_calloc(size_t align, size_t count, size_t bytes, unsigned caps)
{
    if (allocation_fails()) return NULL;
#ifdef KMS_USB_SHARED_HEAP
    (void)caps;
    return kms_shared_aligned(align, count * bytes);
#else
    if (align != 512 || caps != XFER_DESC_LIST_CAPS || !count || bytes > sizeof(dma_heap) / count) {
        ++mistakes; return NULL;
    }
    unsigned length = (unsigned)ALIGN_UP(count * bytes, cache_alignment);
    for (unsigned start = 0; start + length + (unsigned)misalign_dma <= sizeof(dma_heap); start += (unsigned)align) {
        bool available = true;
        for (unsigned j = 0; j < length + (unsigned)misalign_dma; ++j) if (dma_used[start + j]) available = false;
        if (!available) continue;
        for (unsigned n = 0; n < 32; ++n) if (!dma_allocations[n].pointer) {
            unsigned reserved = length + (unsigned)misalign_dma;
            void *result = dma_heap + start + (unsigned)misalign_dma;
            memset(dma_used + start, 1, reserved); memset(result, 0, count * bytes);
            dma_allocations[n].pointer = result; dma_allocations[n].start = start; dma_allocations[n].bytes = reserved;
            return result;
        }
        ++mistakes; return NULL;
    }
    return NULL;
#endif
}
#ifndef KMS_USB_SHARED_HEAP
void free(void *pointer)
{
    if (!pointer) return;
    for (unsigned n = 0; n < 128; ++n) if (pointer == heap[n].bytes && heap[n].used) { heap[n].used = false; return; }
    for (unsigned n = 0; n < 32; ++n) if (pointer == dma_allocations[n].pointer) {
        memset(dma_used + dma_allocations[n].start, 0, dma_allocations[n].bytes);
        dma_allocations[n].pointer = NULL; return;
    }
    ++mistakes;
}
#endif
static void heap_caps_free(void *pointer) { free(pointer); }
static int esp_cache_get_alignment(unsigned caps, size_t *align)
{ if (caps != XFER_DESC_LIST_CAPS) ++mistakes; *align = cache_alignment; return ESP_OK; }
static bool pipe_args_usb_compliance_verification(const hcd_pipe_config_t *p, usb_speed_t speed, usb_transfer_type_t type)
{ (void)p; (void)speed; (void)type; return compliant; }
static bool pipe_alloc_hcd_support_verification(usb_dwc_hal_context_t *h, const usb_ep_desc_t *e)
{ (void)h; (void)e; return supported; }
static void pipe_set_ep_char(const hcd_pipe_config_t *p, usb_transfer_type_t type, bool is_default, int index, usb_speed_t speed, usb_dwc_hal_ep_char_t *out)
{ (void)type; (void)is_default; (void)index; (void)speed; out->mps = p->ep_desc ? p->ep_desc->wMaxPacketSize : 64; }
static bool usb_dwc_hal_chan_alloc(usb_dwc_hal_context_t *h, usb_dwc_hal_chan_t *c, void *context)
{
    (void)context;
    for (unsigned n = 0; n < 8; ++n) if (!(h->occupied & (1u << n))) { h->occupied |= 1u << n; c->channel = n; return true; }
    return false;
}
static void usb_dwc_hal_chan_free(usb_dwc_hal_context_t *h, usb_dwc_hal_chan_t *c)
{ if (!(h->occupied & (1u << c->channel))) ++mistakes; h->occupied &= ~(1u << c->channel); }
static void usb_dwc_hal_chan_set_ep_char(usb_dwc_hal_context_t *h, usb_dwc_hal_chan_t *c, const usb_dwc_hal_ep_char_t *ep)
{ (void)h; (void)c; (void)ep; }

// Generated verbatim function bodies from the installed, pinned USB component.
#include "sdk_allocation_functions.inc"

static unsigned live(void)
{
    unsigned count = 0;
    for (unsigned n = 0; n < 128; ++n) count += heap[n].used;
    for (unsigned n = 0; n < 32; ++n) count += dma_allocations[n].pointer != NULL;
    return count;
}
static void reset(unsigned occupied)
{
    memset(heap, 0, sizeof(heap)); memset(dma_used, 0, sizeof(dma_used)); memset(dma_allocations, 0, sizeof(dma_allocations));
    memset(&fixture_port, 0, sizeof(fixture_port)); hal.occupied = occupied;
    fixture_port.hal = &hal; fixture_port.flags.conn_dev_ena = s_port_inited[0] = compliant = supported = true;
    alloc_calls = fail_at = disconnect_at = mistakes = cache_alignment = 0; misalign_dma = false;
}
#ifdef KMS_USB_SHARED_HEAP
static endpoint_t *shared_endpoints[128][4];
static device_t shared_devices[128];
static usb_ep_desc_t shared_descriptors[128][4];
void kms_shared_sdk_reset(void)
{
    reset(15); // EP0/hub channels are baseline; four input channels remain.
    memset(shared_endpoints, 0, sizeof(shared_endpoints));
}
int kms_shared_sdk_claim(unsigned address, unsigned number)
{
    shared_devices[address].constant.port_hdl = &fixture_port;
    shared_devices[address].constant.address = address;
    shared_descriptors[address][number] = (usb_ep_desc_t){.bmAttributes = USB_TRANSFER_TYPE_INTR, .wMaxPacketSize = 8};
    usbh_ep_config_t config = {0};
    return endpoint_alloc(&shared_devices[address], &shared_descriptors[address][number], &config,
                          &shared_endpoints[address][number]);
}
void kms_shared_sdk_release(unsigned address, unsigned number)
{
    endpoint_free(shared_endpoints[address][number]); shared_endpoints[address][number] = NULL;
}
unsigned kms_shared_sdk_errors(void) { return mistakes; }
#endif
static int allocation_case(unsigned type, unsigned used, unsigned failure, unsigned bad_alignment, unsigned cache)
{
    ++cases; current_case = type | used << 2 | failure << 6 | bad_alignment << 10 | cache << 11;
    unsigned occupied = (1u << used) - 1; reset(occupied);
    fail_at = failure; misalign_dma = bad_alignment != 0; cache_alignment = cache;
    usb_ep_desc_t desc = {.bmAttributes = (uint8_t)type, .wMaxPacketSize = 64};
    device_t device = {.constant = {.port_hdl = &fixture_port, .address = 2}};
    usbh_ep_config_t config = {0}; endpoint_t *endpoint = NULL;
    int result = endpoint_alloc(&device, &desc, &config, &endpoint);
    if (failure || bad_alignment) CHECK(result == ESP_ERR_NO_MEM);
    else if (used == 8) CHECK(result == ESP_ERR_NOT_SUPPORTED); // Exhausted channels are not a memory failure.
    else {
        CHECK(result == ESP_OK && endpoint && live() == 7 && fixture_port.num_pipes_idle == 1);
        pipe_t *pipe = endpoint->constant.pipe_hdl;
        unsigned descriptors = type == 0 ? XFER_LIST_LEN_CTRL : type == 1 ? XFER_LIST_LEN_ISOC : type == 2 ? XFER_LIST_LEN_BULK : XFER_LIST_LEN_INTR;
        for (unsigned n = 0; n < NUM_BUFFERS; ++n) {
            CHECK(((uintptr_t)pipe->buffers[n]->xfer_desc_list & 511) == 0);
            CHECK(pipe->buffers[n]->xfer_desc_list_len_bytes == (int)ALIGN_UP(descriptors * sizeof(usb_dwc_ll_dma_qtd_t), cache));
            const uint8_t *bytes = pipe->buffers[n]->xfer_desc_list;
            for (unsigned j = 0; j < descriptors * sizeof(usb_dwc_ll_dma_qtd_t); ++j) CHECK(bytes[j] == 0);
        }
        pipe->cs_flags.has_urb = true; CHECK(hcd_pipe_free(pipe) == ESP_ERR_INVALID_STATE && live() == 7);
        pipe->cs_flags.has_urb = false; pipe->multi_buffer_control.buffer_is_executing = true;
        CHECK(hcd_pipe_free(pipe) == ESP_ERR_INVALID_STATE && live() == 7);
        pipe->multi_buffer_control.buffer_is_executing = false; endpoint_free(endpoint);
    }
    CHECK(!live() && hal.occupied == occupied && fixture_port.num_pipes_idle == 0 && !fixture_port.pipes_idle_tailq.first && !mistakes);
    return 0;
}
static int fragmentation_case(unsigned shift, unsigned stride)
{
    ++cases; current_case = 0x40000000u | shift | stride << 9; reset(0);
    // Every aligned start is pinned, despite >99% aggregate DMA bytes free.
    // Shift/stride sweep also leaves adequate unaligned holes for 256-byte lists.
    for (unsigned n = 0; n < sizeof(dma_heap); n += stride) dma_used[n + shift] = 1;
    unsigned free_bytes = 0; for (unsigned n = 0; n < sizeof(dma_heap); ++n) free_bytes += !dma_used[n];
    CHECK(free_bytes > sizeof(dma_heap) * 99 / 100);
    usb_ep_desc_t desc = {.bmAttributes = USB_TRANSFER_TYPE_INTR, .wMaxPacketSize = 8};
    device_t device = {.constant = {.port_hdl = &fixture_port, .address = 5}};
    usbh_ep_config_t config = {0}; endpoint_t *endpoint = NULL;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        CHECK(endpoint_alloc(&device, &desc, &config, &endpoint) == ESP_ERR_NO_MEM);
        CHECK(!live() && !hal.occupied && !fixture_port.num_pipes_idle && !mistakes);
    }
    memset(dma_used, 0, sizeof(dma_used));
    CHECK(endpoint_alloc(&device, &desc, &config, &endpoint) == ESP_OK);
    endpoint_free(endpoint); CHECK(!live() && !hal.occupied && !mistakes);
    return 0;
}
static int churn_case(unsigned count, unsigned replacing, unsigned failure)
{
    ++cases; current_case = 0x20000000u | count | replacing << 4 | failure << 8; reset(0);
    device_t devices[8] = {0}; usb_ep_desc_t desc[8] = {0}; endpoint_t *held[8] = {0};
    usbh_ep_config_t config = {0};
    for (unsigned n = 0; n < count; ++n) {
        devices[n].constant.port_hdl = &fixture_port; devices[n].constant.address = n + 1;
        desc[n].bmAttributes = (uint8_t)(n % 4); desc[n].wMaxPacketSize = (uint16_t)(n % 2 ? 8 : 64);
        CHECK(endpoint_alloc(&devices[n], &desc[n], &config, &held[n]) == ESP_OK);
        ((uint8_t *)held[n]->constant.pipe_hdl->buffers[0]->xfer_desc_list)[0] = (uint8_t)(0x80 + n);
    }
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        endpoint_free(held[replacing]); held[replacing] = NULL;
        unsigned occupied = hal.occupied;
        CHECK(live() == (count - 1) * 7);
        fail_at = failure ? alloc_calls + failure : 0;
        int result = endpoint_alloc(&devices[replacing], &desc[replacing], &config, &held[replacing]);
        if (failure) {
            CHECK(result == ESP_ERR_NO_MEM && live() == (count - 1) * 7 && hal.occupied == occupied);
            fail_at = 0;
            CHECK(endpoint_alloc(&devices[replacing], &desc[replacing], &config, &held[replacing]) == ESP_OK);
        } else CHECK(result == ESP_OK);
        for (unsigned n = 0; n < count; ++n) if (n != replacing)
            CHECK(((uint8_t *)held[n]->constant.pipe_hdl->buffers[0]->xfer_desc_list)[0] == 0x80 + n);
        CHECK(live() == count * 7 && fixture_port.num_pipes_idle == (int)count && !mistakes);
    }
    for (unsigned n = 0; n < count; ++n) endpoint_free(held[n]);
    CHECK(!live() && !hal.occupied && !fixture_port.num_pipes_idle && !fixture_port.pipes_idle_tailq.first && !mistakes);
    return 0;
}
__declspec(dllexport) unsigned kms_usb_allocation_cases(void) { return cases; }
__declspec(dllexport) unsigned kms_usb_allocation_checks(void) { return checks; }
__declspec(dllexport) unsigned kms_usb_allocation_case(void) { return current_case; }
__declspec(dllexport) int kms_run_usb_allocation_tests(void)
{
    checks = cases = 0;
    _Static_assert(sizeof(usb_dwc_ll_dma_qtd_t) == 8, "Use S3 target QTD byte size with native pointers adapted");
    for (unsigned type = 0; type < 4; ++type)
        for (unsigned used = 0; used <= 8; ++used)
            for (unsigned failure = 0; failure <= 7; ++failure)
                for (unsigned alignment = 0; alignment < 2; ++alignment)
                    for (unsigned cache = 0; cache <= 32; cache += 32) {
                        int result = allocation_case(type, used, failure, alignment, cache); if (result) return result;
                    }
    for (unsigned shift = 0; shift < 256; shift += 17)
        for (unsigned stride = 256; stride <= 512; stride += 256) {
            int result = fragmentation_case(shift, stride); if (result) return result;
        }
    for (unsigned count = 1; count <= 8; ++count)
        for (unsigned replacing = 0; replacing < count; ++replacing)
            for (unsigned failure = 0; failure <= 7; ++failure) {
                int result = churn_case(count, replacing, failure); if (result) return result;
            }
    for (unsigned cut = 0; cut <= 6; ++cut) {
        ++cases; current_case = 0x80000000u | cut; reset(0);
        if (cut) disconnect_at = cut; else s_port_inited[0] = false;
        hcd_pipe_config_t config = {0}; pipe_t *pipe = NULL;
        CHECK(hcd_pipe_alloc(&fixture_port, &config, &pipe) == ESP_ERR_INVALID_STATE);
        CHECK(!live() && !hal.occupied && !fixture_port.num_pipes_idle && !mistakes);
    }
    return 0;
}
