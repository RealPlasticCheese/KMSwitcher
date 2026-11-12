#include "kms_receiver_recovery.h"
#include <string.h>

void kms_receiver_recovery_boot(kms_receiver_recovery_t *p, uint32_t boot, uint32_t now)
{
    if (p->boot == boot) return;
    p->boot = boot; p->boot_at = now; p->faults = p->reasons = 0; p->fault_seen = false; p->healthy_at = now;
}
void kms_receiver_recovery_fault(kms_receiver_recovery_t *p, uint32_t boot, bool active, unsigned reason, uint32_t now)
{
    kms_receiver_recovery_boot(p, boot, now);
    uint32_t window = active ? KMS_RECOVERY_ACTIVE_WINDOW_MS : KMS_RECOVERY_IDLE_WINDOW_MS;
    if (!p->fault_seen || now - p->first_fault >= window || now - p->last_fault >= KMS_RECOVERY_QUIET_MS) {
        p->first_fault = now; p->faults = p->reasons = 0;
    } else if (now - p->last_fault < KMS_RECOVERY_COALESCE_MS) {
        p->reasons |= reason; p->healthy_at = now; return;
    }
    p->fault_seen = true; p->last_fault = p->healthy_at = now;
    if (p->faults < UINT32_MAX) ++p->faults;
    p->reasons |= reason;
}
bool kms_receiver_recovery_due(const kms_receiver_recovery_t *p, bool active, uint32_t now)
{
    uint32_t window = active ? KMS_RECOVERY_ACTIVE_WINDOW_MS : KMS_RECOVERY_IDLE_WINDOW_MS;
    uint32_t span = active ? KMS_RECOVERY_ACTIVE_SPAN_MS : KMS_RECOVERY_IDLE_SPAN_MS;
    unsigned threshold = active ? KMS_RECOVERY_ACTIVE_FAILURES : KMS_RECOVERY_IDLE_FAILURES;
    unsigned recent = 0;
    for (unsigned i = 0; i < p->attempts; ++i) recent += now - p->attempt_at[i] < KMS_RECOVERY_BUDGET_MS;
    return p->boot && !p->waiting && p->fault_seen && p->faults >= threshold &&
        now - p->boot_at >= KMS_RECOVERY_GRACE_MS &&
        p->last_fault - p->first_fault >= span && now - p->first_fault < window &&
        now - p->last_fault < KMS_RECOVERY_QUIET_MS &&
        (!p->cooldown || (int32_t)(now - p->next_at) >= 0) &&
        recent < KMS_RECOVERY_MAX_ATTEMPTS;
}
void kms_receiver_recovery_begin(kms_receiver_recovery_t *p, uint32_t now)
{
    unsigned recent = 0;
    for (unsigned i = 0; i < p->attempts; ++i)
        if (now - p->attempt_at[i] < KMS_RECOVERY_BUDGET_MS) p->attempt_at[recent++] = p->attempt_at[i];
    if (recent >= KMS_RECOVERY_MAX_ATTEMPTS) return;
    p->attempt_at[recent++] = now; p->attempts = (uint8_t)recent;
    uint32_t delay = KMS_RECOVERY_BACKOFF_MS;
    for (unsigned i = 0; i < p->level && delay < KMS_RECOVERY_BACKOFF_MAX_MS; ++i) delay *= 2;
    if (delay > KMS_RECOVERY_BACKOFF_MAX_MS) delay = KMS_RECOVERY_BACKOFF_MAX_MS;
    if (p->level < 8) ++p->level;
    p->next_at = now + delay; p->cooldown = true; p->waiting = true;
    p->restart_at = now; p->restart_boot = p->boot; p->faults = 0; p->fault_seen = false;
}
bool kms_receiver_recovery_confirm(kms_receiver_recovery_t *p, uint32_t boot, bool confirmed, uint32_t now)
{
    if (!p->waiting || !boot || boot == p->restart_boot || !confirmed) return false;
    p->waiting = false; kms_receiver_recovery_boot(p, boot, now); p->healthy_at = now;
    p->faults = p->reasons = 0; p->fault_seen = false; return true;
}
bool kms_receiver_recovery_expired(kms_receiver_recovery_t *p, uint32_t now)
{
    if (!p->waiting || now - p->restart_at < KMS_RECOVERY_BOOT_WAIT_MS) return false;
    p->waiting = false; return true;
}
void kms_receiver_recovery_healthy(kms_receiver_recovery_t *p, uint32_t now)
{
    if (!p->waiting && now - p->healthy_at >= KMS_RECOVERY_HEALTHY_MS) {
        p->level = 0; p->fault_seen = false; p->faults = p->reasons = 0;
    }
}
void kms_receiver_recovery_restore(kms_receiver_recovery_t *p, const kms_recovery_budget_t *budget, uint32_t now)
{
    p->attempts = budget->attempts; p->level = budget->level;
    if (p->attempts) {
        // No wall clock is assumed: after an S3 reset retain the budget for a
        // fresh full window, and apply the maximum cooldown conservatively.
        p->cooldown = true;
        for (unsigned i = 0; i < p->attempts; ++i) p->attempt_at[i] = now;
        p->next_at = now + KMS_RECOVERY_BACKOFF_MAX_MS;
    }
}
bool kms_recovery_store_encode(bool enabled, const kms_recovery_budget_t *peers, uint8_t *out)
{
    memset(out, 0, KMS_RECOVERY_STORE_BYTES); memcpy(out, "KRX1", 4); out[4] = enabled; out[5] = KMS_SLOTS;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        if (peers[i].attempts > KMS_RECOVERY_MAX_ATTEMPTS || peers[i].level > 8) return false;
        uint8_t *record = out + 8 + i * 16;
        memcpy(record, peers[i].mac, 6); record[6] = peers[i].attempts; record[7] = peers[i].level;
        kms_put_u32(record + 8, peers[i].boot); kms_put_u32(record + 12, peers[i].reasons);
    }
    kms_put_u32(out + KMS_RECOVERY_STORE_BYTES - 4, kms_crc32(out, KMS_RECOVERY_STORE_BYTES - 4)); return true;
}
bool kms_recovery_store_decode(bool *enabled, kms_recovery_budget_t *peers, const uint8_t *in, size_t size)
{
    if (size != KMS_RECOVERY_STORE_BYTES || memcmp(in, "KRX1", 4) || in[4] > 1 || in[5] != KMS_SLOTS || in[6] || in[7] ||
        kms_get_u32(in + size - 4) != kms_crc32(in, size - 4)) return false;
    kms_recovery_budget_t decoded[KMS_SLOTS] = {0};
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        const uint8_t *record = in + 8 + i * 16;
        if (record[6] > KMS_RECOVERY_MAX_ATTEMPTS || record[7] > 8) return false;
        memcpy(decoded[i].mac, record, 6); decoded[i].attempts = record[6]; decoded[i].level = record[7];
        decoded[i].boot = kms_get_u32(record + 8); decoded[i].reasons = kms_get_u32(record + 12);
    }
    memcpy(peers, decoded, sizeof(decoded)); *enabled = in[4] != 0; return true;
}
