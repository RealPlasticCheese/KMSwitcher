#include "usb_bus.h"
#include "kms_runtime.h"
#include "soc/usb_struct.h"
#include "usb_driver_trace.h"

static uint32_t last_sample, last_change;
static uint16_t last_frame;
static bool sampled;
static kms_usb_bus_history_t history;
static unsigned next_history;
static uint32_t last_history;
// Legacy soc/usb_struct.h omits volatile on this hardware register.
static uint32_t clock_state(void) { return *(volatile const uint32_t *)&USB0.pcgctrl; }

static void sample_progress(uint32_t now)
{
    if (history.count && now - last_history < KMS_USB_BUS_SAMPLE_MS) return;
    kms_usb_bus_sample_t *s = &history.samples[next_history];
    s->at = now; s->hfnum = USB0.hfnum; s->gahbcfg = USB0.gahbcfg; s->grstctl = USB0.grstctl;
    s->gnptxsts = USB0.gnptxsts; s->hptxsts = USB0.hptxsts; s->hcfg = USB0.hcfg; s->pcgcctl = clock_state();
    s->gintsts = USB0.gintsts; s->gintmsk = USB0.gintmsk; s->haint = USB0.haint;
    kms_usb_driver_activity(&s->interrupts, &s->last_interrupt);
    for (unsigned n = 0; n < 8; ++n) {
        s->channels[n].characteristics = USB0.host_chan_regs[n * 8];
        s->channels[n].size = USB0.host_chan_regs[n * 8 + 4];
        s->channels[n].dma = USB0.host_chan_regs[n * 8 + 5];
        s->channels[n].buffer = USB0.host_chan_regs[n * 8 + 7];
    }
    last_history = now;
    next_history = (next_history + 1) % KMS_USB_BUS_SAMPLES;
    if (history.count < KMS_USB_BUS_SAMPLES) ++history.count;
}

void kms_usb_bus_sample(void)
{
    uint32_t now = kms_now();
    if (sampled && now - last_sample < 20) return;
    uint16_t frame = (uint16_t)USB0.hfnum;
    if (!sampled || frame != last_frame) last_change = now;
    sampled = true; last_sample = now; last_frame = frame;
    sample_progress(now);
}
void kms_usb_bus_snapshot(kms_usb_bus_snapshot_t *out)
{
    kms_usb_bus_sample();
    out->at = kms_now(); out->last_frame_change = last_change;
    out->hfnum = USB0.hfnum; out->hprt = USB0.hprt;
    out->gintsts = USB0.gintsts; out->gintmsk = USB0.gintmsk; out->haint = USB0.haint;
    out->gahbcfg = USB0.gahbcfg; out->gusbcfg = USB0.gusbcfg; out->grstctl = USB0.grstctl;
    out->gnptxsts = USB0.gnptxsts; out->hptxsts = USB0.hptxsts; out->hcfg = USB0.hcfg;
    out->hfir = USB0.hfir; out->hflbaddr = USB0.hflbaddr; out->pcgcctl = clock_state(); out->haintmsk = USB0.haintmsk;
    for (unsigned n = 0; n < 8; ++n) {
        out->channels[n].characteristics = USB0.host_chan_regs[n * 8];
        out->channels[n].interrupt = USB0.host_chan_regs[n * 8 + 2];
        out->channels[n].mask = USB0.host_chan_regs[n * 8 + 3];
        out->channels[n].size = USB0.host_chan_regs[n * 8 + 4];
        out->channels[n].dma = USB0.host_chan_regs[n * 8 + 5];
        out->channels[n].buffer = USB0.host_chan_regs[n * 8 + 7];
    }
}
void kms_usb_bus_history(kms_usb_bus_history_t *out)
{
    out->count = history.count;
    unsigned oldest = (next_history + KMS_USB_BUS_SAMPLES - history.count) % KMS_USB_BUS_SAMPLES;
    for (unsigned n = 0; n < history.count; ++n)
        out->samples[n] = history.samples[(oldest + n) % KMS_USB_BUS_SAMPLES];
}
