// Exercise the production wrappers with decoders that clear their raw registers.
#include "freertos/FreeRTOS.h"
static unsigned trace_lock_depth, guard_nesting_errors;
#undef portENTER_CRITICAL_SAFE
#undef portEXIT_CRITICAL_SAFE
#define portENTER_CRITICAL_SAFE(p) do { (void)(p); ++trace_lock_depth; } while (0)
#define portEXIT_CRITICAL_SAFE(p) do { (void)(p); --trace_lock_depth; } while (0)
#include "usb_driver_trace.c"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, start_calls, irq_calls, root_calls, dequeue_calls, free_calls;
static uint32_t now;
static usb_dwc_hal_chan_t *forwarded_channel;
static usb_dwc_hal_context_t *forwarded_hal;
static void *forwarded_descriptors, *forwarded_pipe;
static int forwarded_count, forwarded_start;
static usb_dwc_hal_chan_event_t irq_result;
static usb_dwc_hal_port_event_t root_result;
static unsigned guard_snapshots;
void kms_usb_frame_guard_snapshot(kms_usb_frame_guard_stats_t *out)
{
    ++guard_snapshots;
    if (trace_lock_depth) ++guard_nesting_errors;
    *out = (kms_usb_frame_guard_stats_t){.enabled = 1, .activations = 42, .waited = 7};
}
static urb_t *dequeue_result;
static kms_usb_driver_snapshot_t saved, after;
int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
void __real_usb_dwc_hal_chan_activate(usb_dwc_hal_chan_t *c, void *d, int count, int start)
{
    ++start_calls; forwarded_channel = c; forwarded_descriptors = d; forwarded_count = count; forwarded_start = start;
    c->flags.active = 1; c->regs->hcchar_reg.val |= 0x80000000u;
}
usb_dwc_hal_chan_event_t __real_usb_dwc_hal_chan_decode_intr(usb_dwc_hal_chan_t *c)
{ ++irq_calls; forwarded_channel = c; c->regs->hcint_reg.val = 0; c->flags.active = 0; return irq_result; }
usb_dwc_hal_port_event_t __real_usb_dwc_hal_decode_intr(usb_dwc_hal_context_t *hal)
{ ++root_calls; forwarded_hal = hal; hal->dev->gintsts_reg.val = 0; hal->dev->hprt_reg.val = 0; return root_result; }
void __real_usb_dwc_hal_chan_free(usb_dwc_hal_context_t *hal, usb_dwc_hal_chan_t *c)
{ ++free_calls; forwarded_hal = hal; forwarded_channel = c; c->flags.val = 0; }
urb_t *__real_hcd_urb_dequeue(hcd_pipe_handle_t p)
{ ++dequeue_calls; forwarded_pipe = p; return dequeue_result; }
__declspec(dllexport) unsigned kms_usb_driver_test_count(void) { return checks; }
__declspec(dllexport) int kms_run_usb_driver_tests(void)
{
    static int pipe;
    static usb_dwc_ll_dma_qtd_t descriptors[3];
    usb_dwc_host_chan_regs_t regs = {.hcchar_reg = {5u << 22}, .hcint_reg = {3}, .hctsiz_reg = {0x12345678}};
    usb_dwc_hal_chan_t channel = {.regs = &regs, .chan_ctx = &pipe}; channel.flags.chan_idx = 2;
    kms_fake_dwc_t dev = {.gintsts_reg = {0x21000000}, .hprt_reg = {0x21405}};
    usb_dwc_hal_context_t hal = {.dev = &dev};
    descriptors[0].buffer_status_val = 0x87000008;
    descriptors[1].buffer_status_val = 0x86000002;
    descriptors[2].buffer_status_val = 0x86000000;
    now = 0xfffffff0u;
    __wrap_usb_dwc_hal_chan_activate(&channel, &descriptors, 3, 1);
    CHECK(start_calls == 1 && forwarded_channel == &channel && forwarded_descriptors == &descriptors);
    CHECK(forwarded_count == 3 && forwarded_start == 1 && channel.flags.active);
    kms_usb_driver_snapshot(&saved);
    CHECK(guard_snapshots == 1 && saved.frame_guard.enabled == 1);
    CHECK(saved.frame_guard.activations == 42 && saved.frame_guard.waited == 7);
    CHECK(saved.channels[2].starts == 1 && saved.channels[2].last_start == now && saved.event_count == 1);
    CHECK(saved.events[0].extra == 3 && saved.events[0].detail == 1 && saved.events[0].size == 0x12345678);
    CHECK(saved.channels[2].descriptor_count == 3 && saved.channels[2].descriptor_start == 1);
    CHECK(saved.channels[2].descriptor_base == (uint32_t)(uintptr_t)&descriptors);
    CHECK(saved.channels[2].submitted_status[0] == 0x87000008 && saved.channels[2].current_status[0] == 0x87000008);
    descriptors[0].buffer_status_val = 0x07000000; // Simulate DMA returning ownership without an IRQ.
    kms_usb_driver_snapshot(&after);
    CHECK(after.channels[2].current_status[0] == 0x07000000 && after.channels[2].submitted_status[0] == 0x87000008);
    CHECK(saved.channels[2].current_status[0] == 0x87000008 && descriptors[0].buffer_status_val == 0x07000000);
    now += 20; regs.hcint_reg.val = 0x402; irq_result = USB_DWC_HAL_CHAN_EVENT_ERROR;
    CHECK(__wrap_usb_dwc_hal_chan_decode_intr(&channel) == irq_result && irq_calls == 1);
    CHECK(regs.hcint_reg.val == 0 && !channel.flags.active);
    kms_usb_driver_snapshot(&saved);
    CHECK(saved.channels[2].errors == 1 && saved.channels[2].last_error == 4 && saved.channels[2].error_bits == 0x402);
    CHECK(saved.events[1].interrupt == 0x402 && saved.events[1].detail == USB_DWC_HAL_CHAN_EVENT_ERROR);
    CHECK(saved.channels[2].error_characteristics == (0x80000000u | 5u << 22));
    root_result = USB_DWC_HAL_PORT_EVENT_OVRCUR;
    CHECK(__wrap_usb_dwc_hal_decode_intr(&hal) == root_result && forwarded_hal == &hal && root_calls == 1);
    kms_usb_driver_snapshot(&saved);
    CHECK(saved.root_events == 1 && saved.root_bits == 0x21000000 && saved.port_bits == 0x21405);
    CHECK(saved.events[2].kind == KMS_USB_DRIVER_ROOT && saved.events[2].characteristics == 0x21405);
    root_result = USB_DWC_HAL_PORT_EVENT_CHAN; now = 20;
    CHECK(__wrap_usb_dwc_hal_decode_intr(&hal) == root_result);
    kms_usb_driver_snapshot(&after);
    CHECK(after.root_interrupts == 2 && after.last_root_interrupt == 20 && after.event_count == saved.event_count);
    CHECK(after.root_bits == saved.root_bits); // Channel IRQs cannot erase the last root event.
    uint32_t activity_count, activity_at;
    kms_usb_driver_activity(&activity_count, &activity_at);
    CHECK(activity_count == 2 && activity_at == 20);
    // Successful high-rate input is counted, not appended; input errors are retained.
    regs.hcchar_reg.val = 0x80000000u | 3u << 18 | 5u << 22;
    irq_result = USB_DWC_HAL_CHAN_EVENT_CPLT;
    for (unsigned n = 0; n < 100; ++n) {
        now += 1; __wrap_usb_dwc_hal_chan_activate(&channel, &descriptors, 1, 0);
        regs.hcint_reg.val = 3; __wrap_usb_dwc_hal_chan_decode_intr(&channel);
    }
    kms_usb_driver_snapshot(&after);
    CHECK(after.event_count == saved.event_count && after.channels[2].starts == 101 && after.channels[2].interrupts == 101);
    CHECK(after.channels[2].errors == 1 && after.channels[2].error_bits == 0x402);
    CHECK(after.channels[2].descriptor_count == 0 && after.channels[2].current_status[0] == 0); // Never read input buffers.
    irq_result = USB_DWC_HAL_CHAN_EVENT_ERROR; regs.hcint_reg.val = 0x12;
    CHECK(__wrap_usb_dwc_hal_chan_decode_intr(&channel) == irq_result);
    kms_usb_driver_snapshot(&after);
    CHECK(after.event_count == saved.event_count + 1 && after.events[3].interrupt == 0x12);
    // Hub status is read only after the real dequeue and only from complete data.
    uint8_t data[12] = {0xa3, 0, 0, 0, 4, 0, 4, 0, 3, 1, 8, 0};
    urb_t urb = {.transfer = {.data_buffer = data, .data_buffer_size = 12, .num_bytes = 12,
        .actual_num_bytes = 12, .status = USB_TRANSFER_STATUS_COMPLETED}};
    regs.hcchar_reg.val = 5u << 22; __wrap_usb_dwc_hal_chan_activate(&channel, &descriptors, 3, 0);
    dequeue_result = &urb;
    CHECK(__wrap_hcd_urb_dequeue(&pipe) == &urb && forwarded_pipe == &pipe && dequeue_calls == 1);
    kms_usb_driver_snapshot(&saved);
    unsigned last = saved.event_count - 1;
    CHECK(saved.events[last].kind == KMS_USB_DRIVER_HUB_STATUS && saved.events[last].detail == 4);
    CHECK(saved.events[last].extra == 0x00080103 && saved.events[last].channel == 2);
    CHECK(saved.events[last].characteristics == (0x80000000u | 5u << 22));
    CHECK(data[8] == 3 && data[9] == 1 && data[10] == 8); // No buffer writes.
    urb.transfer.actual_num_bytes = 10; __wrap_hcd_urb_dequeue(&pipe); kms_usb_driver_snapshot(&after);
    CHECK(after.events[after.event_count - 1].extra == 0 && after.events[after.event_count - 1].size == 10);
    urb.transfer.actual_num_bytes = 12; urb.transfer.status = USB_TRANSFER_STATUS_CANCELED;
    __wrap_hcd_urb_dequeue(&pipe); kms_usb_driver_snapshot(&after);
    CHECK(after.events[after.event_count - 1].extra == 0);
    unsigned count = after.event_count;
    data[0] = 0x80; __wrap_hcd_urb_dequeue(&pipe); // Standard GET_STATUS is not hub data.
    data[0] = 0xa3; urb.transfer.bEndpointAddress = 0x81; __wrap_hcd_urb_dequeue(&pipe);
    urb.transfer.bEndpointAddress = 0; urb.transfer.data_buffer_size = 8; __wrap_hcd_urb_dequeue(&pipe);
    urb.transfer.data_buffer_size = 12; urb.transfer.num_bytes = 8; __wrap_hcd_urb_dequeue(&pipe);
    dequeue_result = NULL; CHECK(__wrap_hcd_urb_dequeue(&pipe) == NULL);
    kms_usb_driver_snapshot(&after); CHECK(after.event_count == count);
    // Channel recycling cannot give later hub completions an old pipe identity.
    __wrap_usb_dwc_hal_chan_free(&hal, &channel);
    CHECK(free_calls == 1 && forwarded_channel == &channel && forwarded_hal == &hal && channel_pipes[2] == NULL);
    descriptors[0].buffer_status_val = 0xdeadbeef; kms_usb_driver_snapshot(&after);
    CHECK(channel_descriptors[2] == NULL && after.channels[2].descriptor_count == 0 && after.channels[2].current_status[0] == 0);
    __wrap_usb_dwc_hal_chan_activate(&channel, &descriptors, 1, 0); kms_usb_driver_snapshot(&after);
    CHECK(after.channels[2].descriptor_count == 0 && channel_descriptors[2] == NULL); // Unknown list shape is ignored.
    __wrap_usb_dwc_hal_chan_free(&hal, &channel);
    dequeue_result = &urb; urb.transfer.num_bytes = 12;
    __wrap_hcd_urb_dequeue(&pipe); kms_usb_driver_snapshot(&after);
    CHECK(after.events[after.event_count - 1].channel == 255);
    // Ring wrap preserves chronology; copying and later events cannot mutate a frozen snapshot.
    for (unsigned n = 0; n < KMS_USB_DRIVER_EVENTS + 9; ++n) {
        now = 1000 + n; __wrap_usb_dwc_hal_chan_activate(&channel, &descriptors, 3, 0);
    }
    kms_usb_driver_snapshot(&saved);
    CHECK(saved.event_count == KMS_USB_DRIVER_EVENTS && saved.events[0].at == 1009);
    CHECK(saved.events[KMS_USB_DRIVER_EVENTS - 1].at == 1000 + KMS_USB_DRIVER_EVENTS + 8);
    now = 2000; __wrap_usb_dwc_hal_chan_activate(&channel, &descriptors, 3, 0);
    kms_usb_driver_snapshot(&after);
    CHECK(after.events[0].at == 1010 && saved.events[0].at == 1009);
    CHECK(saved.total_events + 1 == after.total_events);
    CHECK(!trace_lock_depth && !guard_nesting_errors);
    return 0;
}
