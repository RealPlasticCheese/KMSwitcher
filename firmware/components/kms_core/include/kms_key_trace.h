#pragma once
#include "kms_core.h"

enum kms_key_trace_stage {
    KMS_KEY_OBSERVED, KMS_KEY_DEFERRED, KMS_KEY_ROUTED, KMS_KEY_QUEUED,
    KMS_KEY_SUBMITTED, KMS_KEY_CONFIRMED, KMS_KEY_CONSUMED, KMS_KEY_MISSING,
    KMS_KEY_UNCONFIRMED
};
enum kms_key_trace_reason {
    KMS_KEY_OK, KMS_KEY_SHORTCUT, KMS_KEY_CONFIGURATION, KMS_KEY_CONSOLE,
    KMS_KEY_HELD_BLOCKED, KMS_KEY_MERGED, KMS_KEY_NO_ROUTE, KMS_KEY_SUPPRESSED,
    KMS_KEY_BUFFER_FULL, KMS_KEY_BUFFER_EXPIRED, KMS_KEY_RECEIVER_RESTART,
    KMS_KEY_ROUTE_STOPPED, KMS_KEY_USB_QUEUE_FULL, KMS_KEY_USB_STALE,
    KMS_KEY_USB_RECOVERY, KMS_KEY_USB_INVALIDATED, KMS_KEY_SOURCE_CAPACITY,
    KMS_KEY_INPUT_OVERFLOW, KMS_KEY_RADIO_BUSY, KMS_KEY_ACK_TIMEOUT,
    KMS_KEY_USB_UNAVAILABLE, KMS_KEY_PEER_LOST, KMS_KEY_ROUTER_DROPPED,
    KMS_KEY_SOURCE_REMOVED, KMS_KEY_TRACE_REASONS
};
// Synchronous callback; state pointers are valid only for its duration. An id
// identifies one delivery sample through queue, submission and final outcome.
typedef struct kms_key_trace {
    const kms_state_t *before, *after;
    uint32_t id, generation, sequence;
    unsigned source, slot;
    enum kms_key_trace_stage stage;
    enum kms_key_trace_reason reason;
} kms_key_trace_t;

typedef void (*kms_key_change_fn)(void *, unsigned page, unsigned usage, bool down);
// Keyboard modifiers use HID page 7 usages e0-e7; consumer page c and system
// page 1 retain their HID usages. Pointer-only reports produce no key records.
void kms_key_changes(const kms_state_t *, const kms_state_t *, kms_key_change_fn, void *);
void kms_key_trace_emit(kms_key_trace_fn, void *, enum kms_key_trace_stage,
                        enum kms_key_trace_reason, unsigned source, unsigned slot,
                        uint32_t id, uint32_t generation, uint32_t sequence,
                        const kms_state_t *, const kms_state_t *);
const char *kms_key_trace_stage_name(enum kms_key_trace_stage);
const char *kms_key_trace_reason_name(enum kms_key_trace_reason);
