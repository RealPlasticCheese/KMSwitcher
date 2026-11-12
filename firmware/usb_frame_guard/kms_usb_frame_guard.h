#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { KMS_USB_GUARD_US = 20, KMS_USB_GUARD_BUDGET_US = 64, KMS_USB_GUARD_MAX_READS = 2048 };
typedef struct {
    uint32_t enabled, activations, waited, timeouts, max_wait_us, crossings;
} kms_usb_frame_guard_stats_t;

void kms_usb_frame_guard_configure(bool enabled);
void kms_usb_frame_guard_reset(void);
void kms_usb_frame_guard_snapshot(kms_usb_frame_guard_stats_t *out);
void kms_usb_frame_guard_before_enable(bool interrupt_in);
void kms_usb_frame_guard_after_enable(bool interrupt_in, uint32_t before, uint32_t after);
