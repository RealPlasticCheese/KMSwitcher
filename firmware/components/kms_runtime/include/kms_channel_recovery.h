#pragma once
#include "kms_desktop.h"
#include "kms_radio.h"

// The controller owns the policy. The receiver only answers authenticated probes.
void kms_channel_recovery_init(void);
bool kms_channel_recovery_monitoring(void);
bool kms_channel_recovery_single_reachable(uint32_t now);
void kms_channel_recovery_peer(unsigned slot, uint32_t boot, uint32_t seen, bool supported);
void kms_channel_recovery_reply(const kms_radio_message_t *, uint32_t received_at);
void kms_channel_recovery_mac(uint32_t token, uint32_t started, uint32_t elapsed, bool failed);
void kms_channel_recovery_input_retry(unsigned slot);
void kms_channel_recovery_tick(uint32_t now, unsigned selected, bool observe, bool idle, bool driver_blocked);
void kms_channel_recovery_cancel(void);
void kms_channel_recovery_driver_stall(void);
void kms_channel_recovery_command(kms_desktop_message_t *);

void kms_link_channel_recovery_pause(void);
void kms_link_channel_recovery_finish(bool restore);
void kms_link_channel_recovery_disown(void);
