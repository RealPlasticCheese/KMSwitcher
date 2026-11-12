#pragma once
#include "kms_management.h"
#include "kms_receiver_recovery.h"
#include "kms_desktop.h"

void kms_remote_init(bool master, uint32_t boot);
// Called on the application task after paired-MAC and boot validation.
void kms_remote_receive(const kms_management_frame_t *);
bool kms_remote_take_outgoing(kms_management_frame_t *);
void kms_remote_peer(unsigned slot, bool paired, uint32_t boot, uint32_t seen, uint8_t flags, const uint8_t *mac);
void kms_remote_tick(unsigned own_slot, bool usb_initialized, bool active);
bool kms_remote_console(const char *line);
bool kms_remote_master_updating(void);
bool kms_remote_transfer_busy(void);
bool kms_remote_restart_pending(void);
bool kms_remote_take_restart(void);
bool kms_remote_recovery_enabled(void);
bool kms_remote_master_restart_pending(void);
bool kms_remote_radio_probe_supported(unsigned slot, uint32_t boot);
void kms_remote_recovery_budget(unsigned slot, const uint8_t mac[6], kms_recovery_budget_t *);
bool kms_remote_recovery_supported(unsigned slot, uint32_t boot, bool *confirmed);
bool kms_remote_recovery_request(unsigned slot, uint32_t boot, const kms_recovery_budget_t *, bool active);
typedef struct { unsigned slot, status; uint32_t boot; } kms_recovery_result_t;
bool kms_remote_take_recovery_result(kms_recovery_result_t *);
bool kms_remote_local_diagnostics(const kms_desktop_message_t *);
// Restricted diagnostics relay; firmware writes stay in the standalone updater.
void kms_remote_desktop(unsigned source, uint32_t boot, kms_desktop_message_t *);
