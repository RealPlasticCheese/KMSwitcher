#include "usb_driver_trace.h"
#include "hal/usb_dwc_hal.h"
#include "hcd.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"

#if ESP_IDF_VERSION_MAJOR != 6 || ESP_IDF_VERSION_MINOR != 1 || !CONFIG_IDF_TARGET_ESP32S3
#error USB trace hooks must be reviewed against the pinned ESP-IDF 6.1 S3 HAL before upgrading
#endif

// Linker hooks observe the pinned driver without changing SDK sources or callbacks.
// All hook storage is fixed internal RAM. No logging, allocation, USB calls or
// payload copies occur under this lock; the HCD may already hold its own lock.
static DRAM_ATTR portMUX_TYPE trace_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR kms_usb_driver_snapshot_t trace;
static DRAM_ATTR void *channel_pipes[KMS_USB_DRIVER_CHANNELS];
static DRAM_ATTR volatile usb_dwc_ll_dma_qtd_t *channel_descriptors[KMS_USB_DRIVER_CHANNELS];
static DRAM_ATTR unsigned next_event;

extern void __real_usb_dwc_hal_chan_activate(usb_dwc_hal_chan_t *, void *, int, int);
extern void __real_usb_dwc_hal_chan_free(usb_dwc_hal_context_t *, usb_dwc_hal_chan_t *);
extern usb_dwc_hal_chan_event_t __real_usb_dwc_hal_chan_decode_intr(usb_dwc_hal_chan_t *);
extern usb_dwc_hal_port_event_t __real_usb_dwc_hal_decode_intr(usb_dwc_hal_context_t *);
extern urb_t *__real_hcd_urb_dequeue(hcd_pipe_handle_t);

static uint32_t IRAM_ATTR trace_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void IRAM_ATTR append(kms_usb_driver_event_t event)
{
    trace.events[next_event] = event;
    next_event = (next_event + 1) % KMS_USB_DRIVER_EVENTS;
    if (trace.event_count < KMS_USB_DRIVER_EVENTS) ++trace.event_count;
    ++trace.total_events;
}
static kms_usb_driver_event_t IRAM_ATTR channel_event(usb_dwc_hal_chan_t *channel, unsigned kind)
{
    return (kms_usb_driver_event_t){.at = trace_now(), .kind = kind, .channel = channel->flags.chan_idx,
        .characteristics = channel->regs->hcchar_reg.val, .interrupt = channel->regs->hcint_reg.val,
        .size = channel->regs->hctsiz_reg.val, .flags = channel->flags.val};
}
void IRAM_ATTR __wrap_usb_dwc_hal_chan_activate(usb_dwc_hal_chan_t *channel, void *descriptors, int count, int start)
{
    bool control = ((channel->regs->hcchar_reg.val >> 18) & 3u) == 0;
    volatile usb_dwc_ll_dma_qtd_t *qtd = control && count == 3 && descriptors ? descriptors : NULL;
    uint32_t submitted[3] = {0};
    if (qtd) for (unsigned n = 0; n < 3; ++n) submitted[n] = qtd[n].buffer_status_val;
    __real_usb_dwc_hal_chan_activate(channel, descriptors, count, start);
    kms_usb_driver_event_t event = channel_event(channel, KMS_USB_DRIVER_START);
    event.extra = (uint32_t)count; event.detail = (uint16_t)start;
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (event.channel < KMS_USB_DRIVER_CHANNELS) {
        kms_usb_driver_channel_t *s = &trace.channels[event.channel];
        ++s->starts; s->last_start = event.at; s->characteristics = event.characteristics; s->flags = event.flags;
        channel_pipes[event.channel] = channel->chan_ctx;
        channel_descriptors[event.channel] = qtd;
        s->descriptor_base = (uint32_t)(uintptr_t)descriptors;
        s->descriptor_count = qtd ? 3 : 0; s->descriptor_start = (uint32_t)start;
        for (unsigned n = 0; n < 3; ++n) s->submitted_status[n] = submitted[n];
    }
    // Count high-rate input activity without letting it erase control/error history.
    if (((event.characteristics >> 18) & 3u) == 0) append(event);
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
usb_dwc_hal_chan_event_t IRAM_ATTR __wrap_usb_dwc_hal_chan_decode_intr(usb_dwc_hal_chan_t *channel)
{
    // Read ordinary status registers BEFORE the real decoder acknowledges them.
    kms_usb_driver_event_t event = channel_event(channel, KMS_USB_DRIVER_IRQ);
    usb_dwc_hal_chan_event_t result = __real_usb_dwc_hal_chan_decode_intr(channel);
    event.detail = (uint16_t)result;
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (event.channel < KMS_USB_DRIVER_CHANNELS) {
        kms_usb_driver_channel_t *s = &trace.channels[event.channel];
        ++s->interrupts; s->last_interrupt = event.at; s->interrupt = event.interrupt;
        s->characteristics = event.characteristics; s->flags = channel->flags.val;
        if (result == USB_DWC_HAL_CHAN_EVENT_ERROR) {
            ++s->errors; s->last_error = event.at; s->error_bits = event.interrupt;
            s->error_characteristics = event.characteristics;
        }
    }
    if (((event.characteristics >> 18) & 3u) == 0 || result == USB_DWC_HAL_CHAN_EVENT_ERROR) append(event);
    portEXIT_CRITICAL_SAFE(&trace_lock);
    return result;
}
void IRAM_ATTR __wrap_usb_dwc_hal_chan_free(usb_dwc_hal_context_t *hal, usb_dwc_hal_chan_t *channel)
{
    kms_usb_driver_event_t event = channel_event(channel, KMS_USB_DRIVER_FREE);
    __real_usb_dwc_hal_chan_free(hal, channel);
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (event.channel < KMS_USB_DRIVER_CHANNELS) {
        channel_pipes[event.channel] = NULL; trace.channels[event.channel].flags = 0;
        channel_descriptors[event.channel] = NULL; trace.channels[event.channel].descriptor_count = 0;
    }
    if (((event.characteristics >> 18) & 3u) == 0) append(event);
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
usb_dwc_hal_port_event_t IRAM_ATTR __wrap_usb_dwc_hal_decode_intr(usb_dwc_hal_context_t *hal)
{
    kms_usb_driver_event_t event = {.at = trace_now(), .kind = KMS_USB_DRIVER_ROOT, .channel = 255,
        .interrupt = hal->dev->gintsts_reg.val, .characteristics = hal->dev->hprt_reg.val};
    usb_dwc_hal_port_event_t result = __real_usb_dwc_hal_decode_intr(hal);
    event.detail = (uint16_t)result;
    portENTER_CRITICAL_SAFE(&trace_lock);
    ++trace.root_interrupts; trace.last_root_interrupt = event.at;
    if (result != USB_DWC_HAL_PORT_EVENT_NONE && result != USB_DWC_HAL_PORT_EVENT_CHAN) {
        ++trace.root_events; trace.last_root_event = event.at;
        trace.root_bits = event.interrupt; trace.port_bits = event.characteristics; append(event);
    }
    portEXIT_CRITICAL_SAFE(&trace_lock);
    return result;
}
urb_t *__wrap_hcd_urb_dequeue(hcd_pipe_handle_t pipe)
{
    urb_t *urb = __real_hcd_urb_dequeue(pipe);
    if (!urb) return NULL;
    const usb_transfer_t *t = &urb->transfer;
    // Only hub/port GET_STATUS responses: never retain keyboard or mouse data.
    if (t->bEndpointAddress || !t->data_buffer || t->data_buffer_size < 12 || t->num_bytes < 12) return urb;
    const uint8_t *p = t->data_buffer;
    if ((p[0] != 0xa3 && p[0] != 0xa0) || p[1] || p[2] || p[3] || p[6] != 4 || p[7]) return urb;
    kms_usb_driver_event_t event = {.at = trace_now(), .kind = KMS_USB_DRIVER_HUB_STATUS, .channel = 255,
        .detail = (uint16_t)(p[4] | p[5] << 8), .interrupt = (uint32_t)t->status,
        .size = (uint32_t)t->actual_num_bytes, .flags = p[0]};
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 12)
        event.extra = (uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
    portENTER_CRITICAL_SAFE(&trace_lock);
    for (unsigned n = 0; n < KMS_USB_DRIVER_CHANNELS; ++n) {
        if (channel_pipes[n] != pipe) continue;
        event.channel = n; event.characteristics = trace.channels[n].characteristics; break;
    }
    append(event);
    portEXIT_CRITICAL_SAFE(&trace_lock);
    return urb;
}
void kms_usb_driver_snapshot(kms_usb_driver_snapshot_t *out)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    *out = trace;
    // The pinned HCD calls chan_free before freeing descriptor storage. Its hook
    // clears this pointer under the same lock before returning to that free.
    // DMA/driver updates can race individual status reads, but storage stays live.
    // Never use the SDK parse helper here: it clears descriptor status.
    for (unsigned c = 0; c < KMS_USB_DRIVER_CHANNELS; ++c)
        for (unsigned n = 0; n < 3; ++n)
            out->channels[c].current_status[n] = channel_descriptors[c] ? channel_descriptors[c][n].buffer_status_val : 0;
    unsigned oldest = (next_event + KMS_USB_DRIVER_EVENTS - trace.event_count) % KMS_USB_DRIVER_EVENTS;
    for (unsigned n = 0; n < trace.event_count; ++n)
        out->events[n] = trace.events[(oldest + n) % KMS_USB_DRIVER_EVENTS];
    portEXIT_CRITICAL_SAFE(&trace_lock);
    // Never nest trace -> guard locks: HAL callers acquire guard before trace.
    kms_usb_frame_guard_snapshot(&out->frame_guard);
}
void kms_usb_driver_activity(uint32_t *interrupts, uint32_t *last_interrupt)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    *interrupts = trace.root_interrupts; *last_interrupt = trace.last_root_interrupt;
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
