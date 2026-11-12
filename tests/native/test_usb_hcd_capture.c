// Execute the actual HCD observer against bounded fake objects/registers.
#define CONFIG_IDF_TARGET_ESP32S3 1
#define FRAME_LIST_LEN 32
#define USB0 iso_capture_usb
#define esp_timer_get_time iso_capture_time
#define iso_driver_sample iso_capture_sample
#define iso_driver_record iso_capture_record
#define iso_driver_activation iso_capture_activation
#define iso_hcd_capture iso_test_hcd_capture
#include "driver_trace.h"
#include "soc/usb_struct.h"
#include <string.h>
static unsigned checks, lock_depth, lock_errors, captured_freeze;
static unsigned qtd_index;
#define HCD_ENTER_CRITICAL() (++lock_depth)
#define HCD_EXIT_CRITICAL() (--lock_depth)
typedef struct { uint32_t marker; } capture_transfer_t;
typedef struct { capture_transfer_t transfer; } urb_t;
typedef struct { uint32_t buffer_status_val; uint8_t *buffer; } usb_dwc_ll_dma_qtd_t;
typedef struct { struct { uint32_t val, chan_idx; } flags; } capture_chan_t;
typedef struct { struct { uint32_t periodic_sched_enabled; } flags; } capture_hal_t;
typedef struct {
    void *frame_list;
    struct { uint32_t periodic_scheduling_enabled; } flags; // Unused upstream HCD field remains zero.
    capture_hal_t *hal;
} capture_port_t;
typedef struct { urb_t *urb; void *xfer_desc_list; int xfer_desc_list_len_bytes; struct { uint32_t val; } flags, status_flags; } dma_buffer_block_t;
typedef struct {
    dma_buffer_block_t *buffers[2];
    struct { unsigned rd_idx; uint32_t val; } multi_buffer_control;
    struct { unsigned dev_addr, bEndpointAddress; } ep_char;
    capture_chan_t *chan_obj;
    capture_port_t *port;
    uint32_t state, num_urb_pending, num_urb_done;
} pipe_t;
static unsigned usb_dwc_hal_chan_get_qtd_idx(capture_chan_t *c) { (void)c; return qtd_index; }
usb_dev_t USB0;
static iso_trace_sample_t captured;
static iso_trace_event_t recorded;
int64_t esp_timer_get_time(void) { return 1234567; }
void iso_driver_sample(const iso_trace_sample_t *s, bool freeze)
{ if (lock_depth != 1) ++lock_errors; captured = *s; captured_freeze = freeze; }
void iso_driver_record(uint32_t kind, uint32_t channel, uint32_t object, uint32_t detail, uint32_t qtd, uint32_t status)
{ recorded = (iso_trace_event_t){.kind=kind,.channel=channel,.object=object,.detail=detail,.qtd=qtd,.status=status}; }
void iso_driver_activation(uint32_t channel, uint32_t object, uint32_t detail, uint32_t qtd, uint32_t status, uint32_t before)
{ recorded = (iso_trace_event_t){.kind=ISO_TRACE_ACTIVATE,.channel=channel,.object=object,.detail=detail,.qtd=qtd,.status=status,.before=before}; }
#ifndef KMS_HCD_CAPTURE_SOURCE
#define KMS_HCD_CAPTURE_SOURCE "../../diagnostics/usb-isolation/trace_hcd.inc"
#endif
#include KMS_HCD_CAPTURE_SOURCE
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_run_hcd_capture_tests(void)
{
    static uint32_t frames[32], saved_frames[32];
    static usb_dwc_ll_dma_qtd_t qtd[2], saved_qtd[2];
    static uint8_t data;
    urb_t urb = {0}; capture_chan_t chan = {.flags = {.val=0x31,.chan_idx=3}};
    capture_hal_t hal = {.flags = {.periodic_sched_enabled=1}};
    capture_port_t port = {.frame_list=frames,.hal=&hal};
    dma_buffer_block_t buffer = {.urb=&urb,.xfer_desc_list=qtd,.xfer_desc_list_len_bytes=sizeof(qtd)};
    pipe_t pipe = {.buffers={&buffer,&buffer},.chan_obj=&chan,.port=&port,
                  .ep_char={8,0x81},.multi_buffer_control={0,0x905}};
    checks = lock_depth = lock_errors = qtd_index = 0;
    memset(iso_trace_pipes,0,sizeof(iso_trace_pipes)); memset(&USB0,0,sizeof(USB0));
    iso_trace_pipes[3] = &pipe;
    for (unsigned n=0;n<32;++n) frames[n] = (n%2) ? 8 : 2;
    USB0.hfnum=31; USB0.host_chan_regs[3*8]=0x820c8840;
    qtd[0]=(usb_dwc_ll_dma_qtd_t){0x86000040,&data}; qtd[1]=(usb_dwc_ll_dma_qtd_t){0x06000009,&data};
    memcpy(saved_frames,frames,sizeof(frames)); memcpy(saved_qtd,qtd,sizeof(qtd));
    iso_hcd_capture(true);
    CHECK(!lock_depth && !lock_errors && captured_freeze);
    CHECK(captured.at==1234567 && captured.channel[3].present && !captured.channel[2].present);
    CHECK(captured.channel[3].endpoint==0x881 && captured.channel[3].status==0x86000040);
    CHECK(captured.channel[3].buffers[0].first==0x86000040 && captured.channel[3].buffers[1].second==0x06000009);
    // Regression: the real HAL scheduler is enabled while the unused HCD bit is zero.
    CHECK(!port.flags.periodic_scheduling_enabled && hal.flags.periodic_sched_enabled);
    CHECK(captured.channel[3].schedule_current==8 && captured.channel[3].schedule_next==2);
    CHECK(captured.channel[3].schedule_mask==0xaaaaaaaau);
    CHECK(!memcmp(frames,saved_frames,sizeof(frames)) && !memcmp(qtd,saved_qtd,sizeof(qtd)));
    qtd_index=1; iso_hcd_capture(false);
    CHECK(captured.channel[3].status==0x06000009 && !captured_freeze);
    qtd_index=2; iso_hcd_capture(false);
    CHECK(!captured.channel[3].qtd && !captured.channel[3].status && !captured.channel[3].data);
    iso_trace_pipe(ISO_TRACE_ACTIVATE,&pipe,&urb,1);
    CHECK(!recorded.qtd && !recorded.status);
    qtd_index=0; iso_trace_pipe(ISO_TRACE_IRQ,&pipe,&urb,3);
    CHECK(recorded.qtd==(uint32_t)(uintptr_t)qtd && recorded.status==0x86000040 && recorded.detail==3);
    iso_trace_activation(&pipe,&urb,1,0xbb801234);
    CHECK(recorded.before==0xbb801234 && recorded.kind==ISO_TRACE_ACTIVATE && recorded.status==0x86000040);
    buffer.urb=NULL; iso_hcd_capture(false);
    CHECK(!captured.channel[3].qtd && !captured.channel[3].urb);
    hal.flags.periodic_sched_enabled=0; iso_hcd_capture(false);
    CHECK(!captured.channel[3].schedule_mask);
    hal.flags.periodic_sched_enabled=1; port.frame_list=NULL; iso_hcd_capture(false);
    CHECK(!captured.channel[3].schedule_mask);
    iso_trace_pipes[3]=NULL; iso_hcd_capture(false);
    CHECK(!captured.channel[3].present && !lock_depth && !lock_errors);
    return 0;
}
__declspec(dllexport) unsigned kms_hcd_capture_check_count(void) { return checks; }
