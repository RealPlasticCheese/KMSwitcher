#pragma once
#include "usb_bus.h"
#include "usb_devices.h"
#include "usb_health.h"
#include "usb_driver_trace.h"
#include "kms_diagnostics.h"

typedef struct kms_usb_fault_snapshot {
    kms_diag_record_t context;
    kms_usb_overdue_t reason;
    kms_usb_bus_snapshot_t bus;
    kms_usb_bus_history_t bus_history;
    kms_usb_inventory_t devices;
    kms_usb_health_trace_t controls;
    kms_usb_driver_snapshot_t driver;
    uint32_t sequence;
} kms_usb_fault_snapshot_t;

void kms_usb_fault_init(void);
void kms_usb_fault_capture(const kms_usb_overdue_t *reason);
const kms_usb_fault_snapshot_t *kms_usb_fault_latest(void);
