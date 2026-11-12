#pragma once
#include <stdint.h>
// Independent RAM frame clock for the legacy register reads in the real hook.
typedef struct { volatile uint32_t hfnum, hfir; } usb_dev_t;
extern usb_dev_t USB0;
