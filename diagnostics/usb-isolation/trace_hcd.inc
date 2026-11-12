// Included after upstream HCD object types. This observer never writes a USB
// register, descriptor or scheduler entry. All accesses hold the HCD lock.
#include "driver_trace.h"
#include "esp_timer.h"
#include "soc/usb_struct.h"
#if !CONFIG_IDF_TARGET_ESP32S3
#error Driver trace is validated only for ESP32-S3
#endif
static pipe_t *iso_trace_pipes[ISO_TRACE_CHANNELS];
static void iso_trace_emit(unsigned kind, pipe_t *pipe, urb_t *urb, uint32_t detail, uint32_t before)
{
    dma_buffer_block_t *b = pipe->buffers[pipe->multi_buffer_control.rd_idx];
    unsigned index = usb_dwc_hal_chan_get_qtd_idx(pipe->chan_obj);
    uint32_t address = 0, status = 0;
    if (b->urb && index < (unsigned)b->xfer_desc_list_len_bytes / sizeof(usb_dwc_ll_dma_qtd_t)) {
        volatile const usb_dwc_ll_dma_qtd_t *qtd = b->xfer_desc_list;
        address = (uint32_t)(uintptr_t)&qtd[index]; status = qtd[index].buffer_status_val;
    }
    uint32_t object = urb ? (uint32_t)(uintptr_t)&urb->transfer : 0;
    if (kind == ISO_TRACE_ACTIVATE)
        iso_driver_activation(pipe->chan_obj->flags.chan_idx, object, detail, address, status, before);
    else
        iso_driver_record(kind, pipe->chan_obj->flags.chan_idx, object, detail, address, status);
}
static void iso_trace_pipe(unsigned kind, pipe_t *pipe, urb_t *urb, uint32_t detail)
{
    iso_trace_emit(kind, pipe, urb, detail, 0);
}
static void iso_trace_activation(pipe_t *pipe, urb_t *urb, uint32_t detail, uint32_t before)
{
    iso_trace_emit(ISO_TRACE_ACTIVATE, pipe, urb, detail, before);
}
void iso_hcd_capture(bool freeze)
{
    // Static scratch space: protected by the HCD lock, avoids a large task stack.
    static iso_trace_sample_t sample;
    HCD_ENTER_CRITICAL();
    memset(&sample, 0, sizeof(sample));
    sample.at = (uint32_t)esp_timer_get_time(); sample.frame = USB0.hfnum;
    sample.core = USB0.gintsts; sample.mask = USB0.gintmsk; sample.channels = USB0.haint;
    for (unsigned n = 0; n < ISO_TRACE_CHANNELS; ++n) {
        pipe_t *p = iso_trace_pipes[n]; if (!p) continue;
        iso_trace_channel_t *c = &sample.channel[n];
        c->present = 1; c->pipe = (uint32_t)(uintptr_t)p; c->state = p->state;
        c->endpoint = ((uint32_t)p->ep_char.dev_addr << 8) | p->ep_char.bEndpointAddress;
        c->hal = p->chan_obj->flags.val; c->multi = p->multi_buffer_control.val;
        c->pending = p->num_urb_pending; c->done = p->num_urb_done;
        c->hcchar = USB0.host_chan_regs[n * 8]; c->irq = USB0.host_chan_regs[n * 8 + 2];
        c->mask = USB0.host_chan_regs[n * 8 + 3]; c->size = USB0.host_chan_regs[n * 8 + 4];
        c->dma = USB0.host_chan_regs[n * 8 + 5]; c->index = usb_dwc_hal_chan_get_qtd_idx(p->chan_obj);
        dma_buffer_block_t *b = p->buffers[p->multi_buffer_control.rd_idx];
        c->list = (uint32_t)(uintptr_t)b->xfer_desc_list;
        c->urb = b->urb ? (uint32_t)(uintptr_t)&b->urb->transfer : 0;
        if (b->urb && c->index < (unsigned)b->xfer_desc_list_len_bytes / sizeof(usb_dwc_ll_dma_qtd_t)) {
            volatile const usb_dwc_ll_dma_qtd_t *qtd = b->xfer_desc_list;
            c->qtd = (uint32_t)(uintptr_t)&qtd[c->index]; c->status = qtd[c->index].buffer_status_val;
            c->data = (uint32_t)(uintptr_t)qtd[c->index].buffer;
        }
        for (unsigned slot=0;slot<2;++slot) {
            dma_buffer_block_t *source=p->buffers[slot];
            iso_trace_buffer_t *target=&c->buffers[slot];
            target->list=(uint32_t)(uintptr_t)source->xfer_desc_list;
            target->urb=source->urb ? (uint32_t)(uintptr_t)&source->urb->transfer : 0;
            target->flags=source->flags.val; target->state=source->status_flags.val;
            target->index=c->index;
            unsigned length=(unsigned)source->xfer_desc_list_len_bytes/sizeof(usb_dwc_ll_dma_qtd_t);
            if (source->urb && length) {
                volatile const usb_dwc_ll_dma_qtd_t *qtd=source->xfer_desc_list;
                target->first=qtd[0].buffer_status_val;
                if (length>1) target->second=qtd[1].buffer_status_val;
                if (c->index<length) { target->current=qtd[c->index].buffer_status_val; target->data=(uint32_t)(uintptr_t)qtd[c->index].buffer; }
            }
        }
        volatile const uint32_t *frames = p->port->frame_list;
        if (frames && p->port->hal->flags.periodic_sched_enabled) {
            unsigned frame = sample.frame & 0xffff;
            c->schedule_current = frames[frame % FRAME_LIST_LEN];
            c->schedule_next = frames[(frame + 1) % FRAME_LIST_LEN];
            for (unsigned f = 0; f < FRAME_LIST_LEN; ++f)
                if (frames[f] & (1u << n)) c->schedule_mask |= 1u << f;
        }
    }
    iso_driver_sample(&sample, freeze);
    HCD_EXIT_CRITICAL();
}
