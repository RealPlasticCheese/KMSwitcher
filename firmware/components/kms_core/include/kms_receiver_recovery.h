#pragma once
#include "kms_management.h"
#include "kms_settings.h"

enum { KMS_RECOVERY_MAC = 1, KMS_RECOVERY_SLOW = 2, KMS_RECOVERY_ACK = 4 };
typedef struct {
    uint32_t boot, boot_at, first_fault, last_fault, reasons, faults, healthy_at;
    uint32_t next_at, restart_at, restart_boot, attempt_at[KMS_RECOVERY_MAX_ATTEMPTS];
    uint8_t attempts, level;
    bool fault_seen, cooldown, waiting;
} kms_receiver_recovery_t;
typedef struct { uint8_t mac[6], attempts, level; uint32_t boot, reasons; } kms_recovery_budget_t;
enum { KMS_RECOVERY_STORE_BYTES = 12 + KMS_SLOTS * 16 };
void kms_receiver_recovery_boot(kms_receiver_recovery_t *, uint32_t boot, uint32_t now);
void kms_receiver_recovery_fault(kms_receiver_recovery_t *, uint32_t boot, bool active, unsigned reason, uint32_t now);
bool kms_receiver_recovery_due(const kms_receiver_recovery_t *, bool active, uint32_t now);
void kms_receiver_recovery_begin(kms_receiver_recovery_t *, uint32_t now);
bool kms_receiver_recovery_confirm(kms_receiver_recovery_t *, uint32_t boot, bool confirmed, uint32_t now);
bool kms_receiver_recovery_expired(kms_receiver_recovery_t *, uint32_t now);
void kms_receiver_recovery_healthy(kms_receiver_recovery_t *, uint32_t now);
void kms_receiver_recovery_restore(kms_receiver_recovery_t *, const kms_recovery_budget_t *, uint32_t now);
bool kms_recovery_store_encode(bool enabled, const kms_recovery_budget_t *, uint8_t *);
bool kms_recovery_store_decode(bool *enabled, kms_recovery_budget_t *, const uint8_t *, size_t);
