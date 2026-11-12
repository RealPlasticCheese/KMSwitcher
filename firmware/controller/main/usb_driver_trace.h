#pragma once
#include <stdint.h>
#include "../../usb_frame_guard/kms_usb_frame_guard.h"

#define KMS_USB_DRIVER_EVENTS 48
#define KMS_USB_DRIVER_CHANNELS 8

enum kms_usb_driver_event { KMS_USB_DRIVER_START = 1, KMS_USB_DRIVER_IRQ,
    KMS_USB_DRIVER_ROOT, KMS_USB_DRIVER_HUB_STATUS, KMS_USB_DRIVER_FREE };
typedef struct {
    uint32_t at, characteristics, interrupt, size, flags, extra;
    uint16_t detail;
    uint8_t kind, channel;
} kms_usb_driver_event_t;
typedef struct {
    uint32_t starts, interrupts, errors, last_start, last_interrupt, last_error;
    uint32_t characteristics, interrupt, error_bits, error_characteristics, flags;
    uint32_t descriptor_base, descriptor_count, descriptor_start;
    uint32_t submitted_status[3], current_status[3];
} kms_usb_driver_channel_t;
typedef struct {
    uint32_t total_events, root_interrupts, last_root_interrupt, root_events;
    uint32_t last_root_event, root_bits, port_bits;
    unsigned event_count;
    kms_usb_driver_channel_t channels[KMS_USB_DRIVER_CHANNELS];
    kms_usb_driver_event_t events[KMS_USB_DRIVER_EVENTS];
    kms_usb_frame_guard_stats_t frame_guard;
} kms_usb_driver_snapshot_t;

// Copies bounded RAM only. Never calls the USB stack or reads/clears hardware.
void kms_usb_driver_snapshot(kms_usb_driver_snapshot_t *out);
void kms_usb_driver_activity(uint32_t *interrupts, uint32_t *last_interrupt);
