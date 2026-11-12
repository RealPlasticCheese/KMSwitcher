#pragma once
#include "kms_diagnostics.h"
#include "kms_key_trace.h"

// True means submitted to the radio; false leaves the packet eligible to retry.
typedef bool (*kms_send_fn)(void *, const kms_packet_t *);
typedef void (*kms_delivery_event_fn)(void *, enum kms_event, unsigned, uint32_t);
typedef struct {
    kms_state_t state;
    uint32_t motion[4], at, trace_id;
} kms_buffered_t;
typedef struct {
    kms_buffered_t *buffer;
    // Submitted samples can only occupy the first batch's worth of slots.
    // Keep their original tags even if reactivation replaces the pending packet.
    struct { uint32_t generation, sequence; } submitted[KMS_INPUT_BATCH_MAX];
    kms_packet_t pending;
    kms_packet_t release;
    kms_state_t current, confirmed;
    kms_state_t trace_base;
    uint32_t next_trace_id;
    enum kms_key_trace_reason trace_fault;
    uint32_t motion[4], confirmed_motion[4];
    uint32_t boot, receiver, generation, sequence, seen, flags, first, sent, snapshot;
    uint32_t recovery_at, replay_at, acknowledged_sequence;
    uint32_t ack_count, ack_rtt_ms, ack_max_ms;
    uint32_t release_first, release_sent;
    uint32_t reported_faults;
    unsigned slot, head, count, pending_count, batch_limit;
    bool has_pending, started, pending_restore, restore, recovering, draining;
    bool need_activate, suppressed, block, have_status, retransmission;
    bool releasing, release_started;
    bool pending_traced;
    kms_send_fn send;
    kms_delivery_event_fn event;
    kms_key_trace_fn trace;
    void *ctx;
} kms_delivery_t;
// The caller owns KMS_RECOVERY_BUFFER_DEPTH entries for the lifetime of the sender.
void kms_delivery_init(kms_delivery_t *, kms_buffered_t *, uint32_t boot, kms_send_fn, kms_delivery_event_fn, void *);
void kms_delivery_select(kms_delivery_t *, unsigned slot, uint32_t receiver, uint32_t now);
void kms_delivery_stop(kms_delivery_t *);
void kms_delivery_input(kms_delivery_t *, const kms_state_t *, int32_t, int32_t, int32_t, int32_t, uint32_t now);
void kms_delivery_receive(kms_delivery_t *, const kms_packet_t *, uint32_t now);
void kms_delivery_fault(kms_delivery_t *, enum kms_event, uint32_t now);
void kms_delivery_abandon(kms_delivery_t *, enum kms_event, uint32_t now);
void kms_delivery_tick(kms_delivery_t *, uint32_t now);
