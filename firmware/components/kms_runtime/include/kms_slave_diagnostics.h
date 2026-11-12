#pragma once
#include "kms_slave_snapshot.h"
#include "kms_management.h"
#include "kms_desktop.h"

// Event/metric calls are RAM-only and safe from the application or USB task.
void kms_slave_diagnostics_init(uint32_t boot);
void kms_slave_diagnostics_event(unsigned event, uint32_t detail);
void kms_slave_diagnostics_add(unsigned metric, uint32_t amount);
void kms_slave_diagnostics_set(unsigned metric, uint32_t value);
void kms_slave_diagnostics_peak(unsigned metric, uint32_t value);
void kms_slave_diagnostics_radio(int rssi, unsigned queue_depth);
void kms_slave_diagnostics_fault(unsigned reason, unsigned op, uint32_t duration, uint32_t failed);
bool kms_slave_diagnostics_before_restart(bool automatic);
void kms_slave_diagnostics_tick(void);
// Only the low-priority management worker calls storage/snapshot operations.
void kms_slave_diagnostics_maintain(void);
bool kms_slave_diagnostics_clear(void);
bool kms_slave_diagnostics_snapshot(kms_slave_snapshot_t *);
size_t kms_slave_diagnostics_summary(uint8_t *out);
void kms_slave_diagnostics_print_summary(const uint8_t *, size_t);
void kms_slave_diagnostics_local(const kms_desktop_message_t *, kms_desktop_message_t *);
bool kms_slave_diagnostics_local_active(void);
