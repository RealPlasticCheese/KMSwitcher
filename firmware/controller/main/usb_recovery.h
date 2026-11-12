#pragma once
#include "kms_core.h"
typedef struct { unsigned state, attempts, recoveries; uint32_t restarts; } kms_usb_recovery_status_t;
void kms_usb_recovery_status_snapshot(kms_usb_recovery_status_t *);
void kms_usb_recovery_print_status(const kms_usb_recovery_status_t *);

void kms_usb_recovery_init(void);
void kms_usb_recovery_tick(bool allowed);
void kms_usb_recovery_watch_begin(bool allowed);
void kms_usb_recovery_watch_end(void);
bool kms_usb_recovery_request(void);
bool kms_usb_recovery_active(void);
unsigned kms_usb_recovery_boot_slot(unsigned configured_slot);
bool kms_usb_recovery_boot_paused(void);
bool kms_usb_recovery_block_initial(void);
void kms_usb_recovery_status(void);
// Requires CONFIG_KMS_TEST_SUPPORT and an explicitly enabled test session.
bool kms_usb_recovery_test_block(void);
