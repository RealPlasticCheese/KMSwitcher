// Run the generated production overlay and shared guard, with real S3 HAL/LL.
#include "hal/usb_dwc_hal.h"
#include "soc/usb_struct.h"
#include "kms_usb_frame_guard.h"
#include <string.h>
usb_dwc_dev_t USB_DWC;
usb_dev_t USB0;
static unsigned checks, cases, assertions, clock_reads;
static uint32_t ticks, now;
static bool stopped;
static usb_dwc_hal_chan_t channels[8], *handles[8];
static usb_dwc_hal_context_t hal;
static usb_dwc_ll_dma_qtd_t qtd[3], original[3];
static uint32_t frames[32];
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
void kms_sdk_assert(void) { ++assertions; }
int kms_sdk_ffs(int value)
{ for (unsigned bit=0; bit<32; ++bit) if ((uint32_t)value & (1u<<bit)) return bit+1; return 0; }
__declspec(noreturn) void abort(void) { for (;;) { } }
int64_t esp_timer_get_time(void)
{
    ++clock_reads; ++now;
    if (!stopped) {
        ticks += USB0.hfir / 1000;
        USB0.hfnum = ((USB0.hfir - ticks % USB0.hfir) << 16) | ((ticks / USB0.hfir) & 0xffff);
    }
    return now;
}
__declspec(dllexport) unsigned kms_guard_hal_check_count(void) { return checks; }
__declspec(dllexport) unsigned kms_guard_hal_case_count(void) { return cases; }
__declspec(dllexport) int kms_run_guard_hal_tests(void)
{
    kms_usb_frame_guard_stats_t stats;
    kms_usb_frame_guard_snapshot(&stats);
    CHECK(stats.enabled == 1); // Production boots protected without an application call.
    for (unsigned speed=0; speed<2; ++speed)
        for (unsigned channel=0; channel<8; ++channel)
            for (unsigned type=0; type<4; ++type)
                for (unsigned input=0; input<2; ++input)
                    for (unsigned enabled=0; enabled<2; ++enabled)
                        for (unsigned phase=0; phase<3; ++phase) {
                            ++cases;
                            memset(&USB_DWC,0,sizeof(USB_DWC)); memset(&hal,0,sizeof(hal));
                            memset(channels,0,sizeof(channels)); memset(handles,0,sizeof(handles));
                            memset(frames,0,sizeof(frames)); memset(qtd,0,sizeof(qtd));
                            assertions=clock_reads=now=0; stopped=phase==2;
                            hal.dev=&USB_DWC; hal.constant_config.chan_num_total=8;
                            hal.channels.hdls=handles; hal.periodic_frame_list=frames;
                            hal.frame_list_len=USB_HAL_FRAME_LIST_LEN_32;
                            hal.flags.fifo_sizes_set=hal.flags.periodic_sched_enabled=1;
                            for (unsigned n=0;n<=channel;++n) CHECK(usb_dwc_hal_chan_alloc(&hal,&channels[n],NULL));
                            usb_dwc_hal_chan_t *c=&channels[channel];
                            c->type=(usb_dwc_xfer_type_t)type;
                            c->regs->hcchar_reg.epdir=input;
                            USB0.hfir=speed ? 6000 : 48000;
                            unsigned remaining=phase==0 ? USB0.hfir/2 : 15;
                            ticks=100*USB0.hfir+USB0.hfir-remaining;
                            USB0.hfnum=(remaining<<16)|100;
                            qtd[0].buffer_status_val=0x86000040;
                            qtd[1].buffer_status_val=0x06000000;
                            memcpy(original,qtd,sizeof(qtd));
                            kms_usb_frame_guard_configure(enabled!=0);
                            usb_dwc_hal_chan_activate(c,qtd,3,1);
                            kms_usb_frame_guard_snapshot(&stats);
                            bool eligible=type==USB_DWC_XFER_TYPE_INTR && input;
                            CHECK(stats.enabled==enabled && stats.activations==eligible);
                            CHECK(stats.waited==(eligible && enabled && phase!=0));
                            CHECK(stats.timeouts==(eligible && enabled && phase==2));
                            CHECK(clock_reads==0 || (eligible && enabled && phase!=0));
                            CHECK(c->flags.active && c->regs->hcchar_reg.chena && !assertions);
                            CHECK(c->regs->hcchar_reg.epdir==input);
                            CHECK(((c->regs->hctsiz_reg.xfersize>>8)&0xff)==2); // Three descriptors, unchanged SDK encoding.
                            CHECK(!memcmp(qtd,original,sizeof(qtd))); // Guard never changes ownership/payload.
                            CHECK(hal.flags.periodic_sched_enabled && !frames[0]);
                            if (eligible && enabled && phase==1) {
                                unsigned left=USB0.hfnum>>16, margin=USB0.hfir*20/1000;
                                CHECK(left>=margin && left<=USB0.hfir-margin && stats.max_wait_us<64);
                            }
                        }
    return 0;
}
