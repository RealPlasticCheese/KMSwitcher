// Unmodified ESP-IDF S3 HAL + LL + SoC register definitions; RAM register bank.
#include "hal/usb_dwc_hal.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
usb_dwc_dev_t USB_DWC;
static unsigned checks, assertions;
static unsigned case_id, actual_schedule, expected_schedule;
static usb_dwc_hal_context_t hal;
static usb_dwc_hal_chan_t channels[8], *handles[8];
static uint32_t frames[32];
static usb_dwc_ll_dma_qtd_t qtd[3];
void kms_sdk_assert(void) { ++assertions; }
int kms_sdk_ffs(int value)
{ for (unsigned bit = 0; bit < 32; ++bit) if ((uint32_t)value & (1u << bit)) return bit + 1; return 0; }
__declspec(noreturn) void abort(void) { for (;;) { } }
static void setup(void)
{
    memset(&USB_DWC, 0, sizeof(USB_DWC)); memset(&hal, 0, sizeof(hal));
    memset(channels, 0, sizeof(channels)); memset(handles, 0, sizeof(handles));
    memset(frames, 0, sizeof(frames)); memset(qtd, 0, sizeof(qtd)); assertions = 0;
    hal.dev = &USB_DWC; hal.constant_config.chan_num_total = 8;
    hal.channels.hdls = handles; hal.periodic_frame_list = frames; hal.frame_list_len = USB_HAL_FRAME_LIST_LEN_32;
    hal.flags.fifo_sizes_set = hal.flags.periodic_sched_enabled = 1;
}
static int channel_matrix(void)
{
    const uint32_t errors[] = {USB_DWC_LL_INTR_CHAN_STALL, USB_DWC_LL_INTR_CHAN_BBLEER,
        USB_DWC_LL_INTR_CHAN_BNAINTR, USB_DWC_LL_INTR_CHAN_XCS_XACT_ERR};
    const usb_dwc_hal_chan_error_t expected[] = {USB_DWC_HAL_CHAN_ERROR_STALL, USB_DWC_HAL_CHAN_ERROR_PKT_BBL,
        USB_DWC_HAL_CHAN_ERROR_BNA, USB_DWC_HAL_CHAN_ERROR_XCS_XACT};
    for (unsigned channel = 0; channel < 8; ++channel)
        for (unsigned halt = 0; halt < 2; ++halt)
            for (unsigned error = 0; error < 4; ++error) {
                setup(); usb_dwc_hal_chan_t *c = &channels[channel];
                for (unsigned n = 0; n <= channel; ++n) CHECK(usb_dwc_hal_chan_alloc(&hal, &channels[n], NULL));
                usb_dwc_hal_chan_activate(c, qtd, 3, 0);
                c->flags.halt_requested = halt;
                c->regs->hcint_reg.val = errors[error] | USB_DWC_LL_INTR_CHAN_CHHLTD | USB_DWC_LL_INTR_CHAN_XFERCOMPL;
                CHECK(usb_dwc_hal_chan_decode_intr(c) == USB_DWC_HAL_CHAN_EVENT_ERROR);
                CHECK(!c->flags.active && c->error == expected[error] && !assertions);
            }
    // Normal completion, requested halt and short-packet two-interrupt sequence.
    for (unsigned type = 0; type < 4; ++type)
        for (unsigned halt = 0; halt < 2; ++halt) {
            setup(); usb_dwc_hal_chan_t *c = &channels[0];
            CHECK(usb_dwc_hal_chan_alloc(&hal, c, NULL)); c->type = (usb_dwc_xfer_type_t)type;
            usb_dwc_hal_chan_activate(c, qtd, 1, 0);
            if (halt) CHECK(!usb_dwc_hal_chan_request_halt(c));
            c->regs->hcint_reg.val = USB_DWC_LL_INTR_CHAN_CHHLTD | USB_DWC_LL_INTR_CHAN_XFERCOMPL;
            CHECK(usb_dwc_hal_chan_decode_intr(c) == (halt ? USB_DWC_HAL_CHAN_EVENT_HALT_REQ : USB_DWC_HAL_CHAN_EVENT_CPLT));
            CHECK(!c->flags.active && !c->flags.halt_requested);
            CHECK(usb_dwc_hal_chan_request_halt(c));
            CHECK(!assertions);
        }
    setup(); usb_dwc_hal_chan_t *c = &channels[0]; CHECK(usb_dwc_hal_chan_alloc(&hal, c, NULL));
    c->type = USB_DWC_XFER_TYPE_INTR; usb_dwc_hal_chan_activate(c, qtd, 3, 0);
    c->regs->hcint_reg.val = USB_DWC_LL_INTR_CHAN_XFERCOMPL;
    CHECK(usb_dwc_hal_chan_decode_intr(c) == USB_DWC_HAL_CHAN_EVENT_NONE);
    CHECK(c->flags.active && c->regs->hcchar_reg.chdis);
    c->regs->hcint_reg.val = USB_DWC_LL_INTR_CHAN_CHHLTD;
    CHECK(usb_dwc_hal_chan_decode_intr(c) == USB_DWC_HAL_CHAN_EVENT_CPLT && !c->flags.active);
    CHECK(!assertions);
    return 0;
}
static int simultaneous_and_stalled(void)
{
    // Every nonempty subset of the eight host channels must be delivered once.
    for (unsigned mask = 1; mask < 256; ++mask) {
        setup(); for (unsigned n = 0; n < 8; ++n) CHECK(usb_dwc_hal_chan_alloc(&hal, &channels[n], NULL));
        USB_DWC.gintsts_reg.val = USB_DWC_LL_INTR_CORE_HCHINT; USB_DWC.haint_reg.val = mask;
        CHECK(usb_dwc_hal_decode_intr(&hal) == USB_DWC_HAL_PORT_EVENT_CHAN);
        unsigned seen = 0; usb_dwc_hal_chan_t *c;
        while ((c = usb_dwc_hal_get_chan_pending_intr(&hal)) != NULL) {
            CHECK(!(seen & (1u << c->flags.chan_idx))); seen |= 1u << c->flags.chan_idx;
        }
        CHECK(seen == mask && !hal.channels.chan_pend_intrs_msk && !assertions);
    }
    // Replay only the *observed register signature*, not the physical DMA cause:
    // RX FIFO nonempty + running frames + no channel interrupt cannot complete a
    // transfer in this HAL. Upper layers must detect a lack of forward progress.
    setup(); CHECK(usb_dwc_hal_chan_alloc(&hal, &channels[0], NULL));
    usb_dwc_hal_chan_activate(&channels[0], qtd, 3, 0);
    for (unsigned frame = 0; frame < 32; ++frame) {
        USB_DWC.gintsts_reg.val = USB_DWC_LL_INTR_CORE_RXFLVL;
        USB_DWC.hfnum_reg.val = frame;
        CHECK(usb_dwc_hal_decode_intr(&hal) == USB_DWC_HAL_PORT_EVENT_NONE);
        CHECK(channels[0].flags.active && !hal.channels.chan_pend_intrs_msk && !assertions);
    }
    return 0;
}
static int schedule_and_reuse(void)
{
    const unsigned intervals[] = {1, 2, 3, 4, 8, 16, 32, 64, 255};
    for (unsigned interval = 0; interval < sizeof(intervals) / sizeof(intervals[0]); ++interval)
        for (unsigned offset = 0; offset < 4; ++offset) {
            setup();
            for (unsigned n = 0; n < 8; ++n) {
                CHECK(usb_dwc_hal_chan_alloc(&hal, &channels[n], NULL));
                usb_dwc_hal_ep_char_t ep = {0};
                ep.dev_addr = n + 1; ep.bEndpointAddress = 0x81; ep.mps = 8;
                ep.type = n < 3 ? USB_DWC_XFER_TYPE_CTRL : USB_DWC_XFER_TYPE_INTR;
                ep.ls_via_fs_hub = n % 2 != 0;
                ep.periodic.interval = intervals[interval]; ep.periodic.offset = offset;
                usb_dwc_hal_chan_set_ep_char(&hal, &channels[n], &ep);
            }
            usb_dwc_hal_chan_t extra;
            CHECK(!usb_dwc_hal_chan_alloc(&hal, &extra, NULL));
            unsigned rounded = 1;
            while (rounded < 32 && rounded * 2 <= intervals[interval]) rounded *= 2;
            for (unsigned f = 0; f < 32; ++f) {
                bool scheduled = ((f + 32 - offset) % rounded) == 0;
                case_id = interval << 16 | offset << 8 | f;
                actual_schedule = frames[f]; expected_schedule = scheduled ? 0xf8u : 0;
                CHECK(frames[f] == (scheduled ? 0xf8u : 0));
            }
            // Repeatedly recycle the hub/input channels at full capacity.
            for (unsigned cycle = 0; cycle < 3; ++cycle)
                for (unsigned n = 3; n < 8; ++n) {
                    usb_dwc_hal_chan_free(&hal, &channels[n]);
                    for (unsigned f = 0; f < 32; ++f) CHECK(!(frames[f] & (1u << n)));
                    CHECK(usb_dwc_hal_chan_alloc(&hal, &channels[n], NULL));
                    CHECK(channels[n].flags.chan_idx == n && !channels[n].flags.active);
                }
            CHECK(hal.channels.num_allocated == 8 && !assertions);
        }
    return 0;
}
static int root_event_priority(void)
{
    for (unsigned overcurrent = 0; overcurrent < 2; ++overcurrent)
        for (unsigned disconnect = 0; disconnect < 2; ++disconnect) {
            setup();
            USB_DWC.gintsts_reg.val = USB_DWC_LL_INTR_CORE_HCHINT | USB_DWC_LL_INTR_CORE_PRTINT |
                (disconnect ? USB_DWC_LL_INTR_CORE_DISCONNINT : 0);
            USB_DWC.haint_reg.val = 0xff;
            USB_DWC.hprt_reg.val = USB_DWC_LL_INTR_HPRT_PRTOVRCURRCHNG | (overcurrent ? 0x10u : 0);
            usb_dwc_hal_port_event_t expected = disconnect ? USB_DWC_HAL_PORT_EVENT_DISCONN :
                overcurrent ? USB_DWC_HAL_PORT_EVENT_OVRCUR : USB_DWC_HAL_PORT_EVENT_OVRCUR_CLR;
            CHECK(usb_dwc_hal_decode_intr(&hal) == expected);
            CHECK(!hal.channels.chan_pend_intrs_msk && !assertions);
        }
    return 0;
}
__declspec(dllexport) int kms_run_usb_stack_tests(void)
{
    checks = 0; int failure = channel_matrix(); if (failure) return failure;
    failure = simultaneous_and_stalled(); if (failure) return failure;
    failure = schedule_and_reuse(); if (failure) return failure;
    return root_event_priority();
}
__declspec(dllexport) unsigned kms_usb_stack_test_count(void) { return checks; }
__declspec(dllexport) unsigned kms_usb_stack_case(void) { return case_id; }
__declspec(dllexport) unsigned kms_usb_stack_actual(void) { return actual_schedule; }
__declspec(dllexport) unsigned kms_usb_stack_expected(void) { return expected_schedule; }
