// Compatibility shim for the standalone reproducer and its native clock model.
#include "frame_guard.h"
#ifdef ISO_GUARD_FRAME
#define KMS_USB_GUARD_FRAME() ISO_GUARD_FRAME()
#endif
#ifdef ISO_GUARD_TIME
#define KMS_USB_GUARD_TIME() ISO_GUARD_TIME()
#endif
#include "../../../firmware/usb_frame_guard/kms_usb_frame_guard.c"
