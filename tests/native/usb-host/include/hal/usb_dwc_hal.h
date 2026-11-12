#pragma once
#include <stdint.h>
typedef struct { uint32_t val; } kms_fake_register_t;
typedef struct { uint32_t buffer_status_val; uint8_t *buffer; } usb_dwc_ll_dma_qtd_t;
typedef struct { kms_fake_register_t hcchar_reg, hcint_reg, hctsiz_reg; } usb_dwc_host_chan_regs_t;
typedef struct { kms_fake_register_t gintsts_reg, hprt_reg; } kms_fake_dwc_t;
typedef struct { kms_fake_dwc_t *dev; } usb_dwc_hal_context_t;
typedef struct {
    union { struct { uint32_t active:1, halt_requested:1, reserved:2, chan_idx:4, reserved24:24; }; uint32_t val; } flags;
    usb_dwc_host_chan_regs_t *regs;
    void *chan_ctx;
} usb_dwc_hal_chan_t;
typedef enum { USB_DWC_HAL_CHAN_EVENT_CPLT, USB_DWC_HAL_CHAN_EVENT_ERROR,
    USB_DWC_HAL_CHAN_EVENT_HALT_REQ, USB_DWC_HAL_CHAN_EVENT_NONE } usb_dwc_hal_chan_event_t;
typedef enum { USB_DWC_HAL_PORT_EVENT_NONE, USB_DWC_HAL_PORT_EVENT_CHAN, USB_DWC_HAL_PORT_EVENT_CONN,
    USB_DWC_HAL_PORT_EVENT_DISCONN, USB_DWC_HAL_PORT_EVENT_ENABLED, USB_DWC_HAL_PORT_EVENT_DISABLED,
    USB_DWC_HAL_PORT_EVENT_OVRCUR, USB_DWC_HAL_PORT_EVENT_OVRCUR_CLR,
    USB_DWC_HAL_PORT_EVENT_REMOTE_WAKEUP } usb_dwc_hal_port_event_t;
