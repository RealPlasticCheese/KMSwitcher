#pragma once
#include <stdint.h>
typedef struct {
    volatile uint32_t hfnum, hprt, gintsts, gintmsk, haint;
    volatile uint32_t gahbcfg, gusbcfg, grstctl, gnptxsts, hptxsts, hcfg, hfir, hflbaddr, pcgctrl, haintmsk;
    volatile uint32_t host_chan_regs[128];
    volatile uint32_t dsts, daint, daintmsk, diepmsk, dctl;
    struct { volatile uint32_t diepctl, diepint, dieptsiz, diepdma, dtxfsts; } in_ep_reg[8];
} usb_dev_t;
extern usb_dev_t USB0;
