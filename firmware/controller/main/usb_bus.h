#pragma once
#include <stdint.h>

#define KMS_USB_BUS_SAMPLES 6
#define KMS_USB_BUS_SAMPLE_MS 500
typedef struct {
    uint32_t at, hfnum, gahbcfg, grstctl, gnptxsts, hptxsts, hcfg, pcgcctl;
    uint32_t gintsts, gintmsk, haint, interrupts, last_interrupt;
    struct { uint32_t characteristics, size, dma, buffer; } channels[8];
} kms_usb_bus_sample_t;
typedef struct {
    unsigned count;
    kms_usb_bus_sample_t samples[KMS_USB_BUS_SAMPLES];
} kms_usb_bus_history_t;

typedef struct {
    uint32_t at, last_frame_change, hfnum, hprt, gintsts, gintmsk, haint;
    uint32_t gahbcfg, gusbcfg, grstctl, gnptxsts, hptxsts, hcfg, hfir, hflbaddr, pcgcctl, haintmsk;
    struct { uint32_t characteristics, interrupt, mask, size, dma, buffer; } channels[8];
} kms_usb_bus_snapshot_t;

// Read-only S3 DWC registers; never read FIFO/pop registers or clear interrupts.
void kms_usb_bus_sample(void);
void kms_usb_bus_snapshot(kms_usb_bus_snapshot_t *out);
void kms_usb_bus_history(kms_usb_bus_history_t *out);
