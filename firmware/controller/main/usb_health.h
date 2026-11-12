#pragma once
#include "kms_core.h"

enum kms_usb_operation { KMS_USB_DESCRIPTOR, KMS_USB_PROTOCOL, KMS_USB_LED, KMS_USB_PROBE, KMS_USB_OPERATIONS };
typedef struct {
    uint32_t started;
    uint8_t address, interface_number, operation;
} kms_usb_overdue_t;

enum { KMS_USB_CONTROL_WATCHES = KMS_USB_HID_INTERFACES + 8, KMS_USB_RECENT_CONTROLS = 8 };
typedef struct {
    uint32_t started, finished;
    int status, expected, actual;
    uint8_t operation, address, interface_number, setup[8];
    bool rejected;
} kms_usb_control_trace_t;
typedef struct {
    kms_usb_control_trace_t pending[KMS_USB_CONTROL_WATCHES], recent[KMS_USB_RECENT_CONTROLS];
    unsigned pending_count, recent_count;
} kms_usb_health_trace_t;

typedef struct { uint32_t completed, failed, max_ms, bins[6]; } kms_usb_timing_t;
typedef struct { kms_usb_timing_t timings[KMS_USB_OPERATIONS]; unsigned pending; } kms_usb_health_status_t;
void kms_usb_health_status_snapshot(kms_usb_health_status_t *);
void kms_usb_health_print_status(const kms_usb_health_status_t *);
// USB-owner task only. A watch owns no transfer memory and ends only on callback.
void kms_usb_health_init(void);
void kms_usb_health_begin(const void *owner, unsigned operation, unsigned address, unsigned interface_number);
void kms_usb_health_request(const void *owner, const void *setup, int expected);
void kms_usb_health_complete(const void *owner, int status, int actual, int expected);
void kms_usb_health_rejected(unsigned operation, unsigned address, unsigned interface_number, const void *setup, int error);
void kms_usb_health_snapshot(kms_usb_health_trace_t *out);
void kms_usb_health_fault(unsigned operation, unsigned address, unsigned interface_number);
void kms_usb_health_clear_fault(void);
bool kms_usb_health_overdue(uint32_t now, uint32_t timeout_ms, kms_usb_overdue_t *out);
unsigned kms_usb_health_pending(void);
void kms_usb_health_status(void);
