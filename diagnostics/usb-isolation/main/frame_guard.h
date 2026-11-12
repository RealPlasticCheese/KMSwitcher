#pragma once
// Preserve the diagnostic API/protocol while sharing the production guard.
#include "../../../firmware/usb_frame_guard/kms_usb_frame_guard.h"
typedef kms_usb_frame_guard_stats_t iso_guard_stats_t;
#define ISO_GUARD_US KMS_USB_GUARD_US
#define ISO_GUARD_BUDGET_US KMS_USB_GUARD_BUDGET_US
#define ISO_GUARD_MAX_READS KMS_USB_GUARD_MAX_READS
#define iso_guard_configure kms_usb_frame_guard_configure
#define iso_guard_reset kms_usb_frame_guard_reset
#define iso_guard_snapshot kms_usb_frame_guard_snapshot
#define iso_guard_before_enable kms_usb_frame_guard_before_enable
#define iso_guard_after_enable kms_usb_frame_guard_after_enable
