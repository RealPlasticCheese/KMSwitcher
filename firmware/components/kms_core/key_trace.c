#include "kms_key_trace.h"

static bool contains(const uint16_t *values, unsigned usage)
{
    for (unsigned n = 0; n < KMS_CONSUMERS; ++n) if (values[n] == usage) return true;
    return false;
}
void kms_key_changes(const kms_state_t *before, const kms_state_t *after, kms_key_change_fn fn, void *ctx)
{
    if (!fn) return;
    for (unsigned byte = 0; byte < KMS_KEY_BYTES; ++byte) {
        unsigned changed = before->keys[byte] ^ after->keys[byte];
        if (!changed) continue;
        for (unsigned bit = 0; bit < 8; ++bit)
            if (changed & (1u << bit)) fn(ctx, 7, byte * 8 + bit, (after->keys[byte] & (1u << bit)) != 0);
    }
    for (unsigned bit = 0; bit < 8; ++bit) {
        unsigned mask = 1u << bit;
        if ((before->modifiers ^ after->modifiers) & mask)
            fn(ctx, 7, 0xe0u + bit, (after->modifiers & mask) != 0);
        if ((before->system ^ after->system) & mask)
            fn(ctx, 1, 0x81u + bit, (after->system & mask) != 0);
    }
    for (unsigned n = 0; n < KMS_CONSUMERS; ++n) {
        unsigned released = before->consumer[n], pressed = after->consumer[n];
        bool duplicate_before = false, duplicate_after = false;
        for (unsigned p = 0; p < n; ++p) {
            duplicate_before |= before->consumer[p] == released;
            duplicate_after |= after->consumer[p] == pressed;
        }
        if (released && !duplicate_before && !contains(after->consumer, released)) fn(ctx, 0xc, released, false);
        if (pressed && !duplicate_after && !contains(before->consumer, pressed)) fn(ctx, 0xc, pressed, true);
    }
}
void kms_key_trace_emit(kms_key_trace_fn fn, void *ctx, enum kms_key_trace_stage stage,
                        enum kms_key_trace_reason reason, unsigned source, unsigned slot,
                        uint32_t id, uint32_t generation, uint32_t sequence,
                        const kms_state_t *before, const kms_state_t *after)
{
    if (!fn) return;
    kms_key_trace_t trace = {.before = before, .after = after, .id = id,
        .generation = generation, .sequence = sequence, .source = source, .slot = slot,
        .stage = stage, .reason = reason};
    fn(ctx, &trace);
}
const char *kms_key_trace_stage_name(enum kms_key_trace_stage stage)
{
    static const char *const names[] = {"observed", "deferred", "routed", "queued", "submitted",
        "confirmed", "consumed", "missing", "unconfirmed"};
    return (unsigned)stage < sizeof(names) / sizeof(*names) ? names[stage] : "unknown";
}
const char *kms_key_trace_reason_name(enum kms_key_trace_reason reason)
{
    static const char *const names[] = {"none", "local-shortcut", "trigger-configuration", "keyboard-console",
        "held-until-release", "held-by-another-input", "no-selected-dongle", "recovery-suppressed",
        "delivery-buffer-full", "recovery-buffer-expired", "receiver-restarted-receipt-unknown",
        "route-stopped", "usb-report-queue-full", "usb-report-too-old", "usb-recovery",
        "usb-source-or-revision-invalidated", "report-source-capacity", "input-overflow",
        "radio-never-accepted-packet", "dongle-ack-timeout", "destination-usb-unavailable", "destination-peer-lost",
        "router-did-not-forward-key", "input-source-removed"};
    return (unsigned)reason < sizeof(names) / sizeof(*names) ? names[reason] : "unknown";
}
