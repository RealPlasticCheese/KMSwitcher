#include "kms_delivery.h"
#include <string.h>

static void notice(kms_delivery_t *d, enum kms_event e, uint32_t detail)
{ if (d->event) d->event(d->ctx, e, d->slot, detail); }
static enum kms_key_trace_reason fault_reason(enum kms_event event)
{
    switch (event) {
    case KMS_EVENT_ACK_TIMEOUT: return KMS_KEY_ACK_TIMEOUT;
    case KMS_EVENT_PEER_LOST: return KMS_KEY_PEER_LOST;
    case KMS_EVENT_PEER_RESTART: return KMS_KEY_RECEIVER_RESTART;
    case KMS_EVENT_USB_UNAVAILABLE: return KMS_KEY_USB_UNAVAILABLE;
    case KMS_EVENT_INPUT_OVERFLOW: return KMS_KEY_INPUT_OVERFLOW;
    default: return KMS_KEY_BUFFER_EXPIRED;
    }
}
static void trace_buffer(kms_delivery_t *d, enum kms_key_trace_stage stage,
                         enum kms_key_trace_reason reason, unsigned count)
{
    if (!d->trace) return;
    const kms_state_t *before = &d->trace_base;
    for (unsigned n = 0; n < count; ++n) {
        const kms_buffered_t *b = &d->buffer[(d->head + n) % KMS_RECOVERY_BUFFER_DEPTH];
        uint32_t sequence = n < KMS_INPUT_BATCH_MAX ? d->submitted[n].sequence : 0;
        bool submitted = sequence != 0;
        enum kms_key_trace_stage outcome = stage;
        if (stage == KMS_KEY_MISSING && submitted) outcome = KMS_KEY_UNCONFIRMED;
        enum kms_key_trace_reason cause = reason;
        if (stage == KMS_KEY_MISSING && !submitted && reason == KMS_KEY_ACK_TIMEOUT &&
            d->has_pending && d->pending_count && !d->pending_traced) cause = KMS_KEY_RADIO_BUSY;
        if (b->trace_id) kms_key_trace_emit(d->trace, d->ctx, outcome, cause, 0, d->slot,
             b->trace_id, submitted ? d->submitted[n].generation : d->generation,
             sequence, before, &b->state);
        before = &b->state;
    }
}
static void retire_trace(kms_delivery_t *d, unsigned count)
{
    if (!count) return;
    d->trace_base = d->buffer[(d->head + count - 1) % KMS_RECOVERY_BUFFER_DEPTH].state;
    unsigned remaining = KMS_INPUT_BATCH_MAX - count;
    for (unsigned n = 0; n < remaining; ++n) d->submitted[n] = d->submitted[n + count];
    memset(d->submitted + remaining, 0, count * sizeof(d->submitted[0]));
}
void kms_delivery_init(kms_delivery_t *d, kms_buffered_t *buffer, uint32_t boot,
                       kms_send_fn send, kms_delivery_event_fn event, void *ctx)
{
    memset(d, 0, sizeof(*d));
    d->buffer = buffer; d->boot = boot; d->send = send; d->event = event; d->ctx = ctx;
}
static void packet(kms_delivery_t *d, unsigned type)
{
    if (!++d->sequence) ++d->sequence;
    d->pending = (kms_packet_t){.type = (uint8_t)type, .slot = (uint8_t)d->slot,
        .session = d->boot, .receiver = d->receiver, .generation = d->generation, .sequence = d->sequence};
    d->has_pending = true; d->started = d->pending_restore = d->retransmission = false;
    d->pending_traced = false;
    d->pending_count = 0;
}
void kms_delivery_stop(kms_delivery_t *d)
{
    trace_buffer(d, KMS_KEY_MISSING, KMS_KEY_ROUTE_STOPPED, d->count);
    // Keep one acknowledged release ahead of the next activation, just as for
    // ordinary switching. An offline old receiver only delays it by the bounded
    // retry window; its fail-safe lease remains the final fallback.
    if (d->slot) {
        packet(d, KMS_RELEASE); d->release = d->pending;
        d->releasing = true; d->release_started = false;
    }
    d->slot = d->head = d->count = 0;
    d->pending_count = 0; d->batch_limit = 1;
    d->has_pending = d->recovering = d->draining = d->suppressed = d->restore = false;
    // A queued block belongs to the discarded route. The caller blocks held
    // input when changing routes; carrying this request over would suppress
    // fresh reports arriving in the same USB batch after a successful switch.
    d->need_activate = d->have_status = d->block = false;
    d->acknowledged_sequence = 0;
    memset(&d->current, 0, sizeof(d->current)); memset(&d->confirmed, 0, sizeof(d->confirmed));
    memset(d->motion, 0, sizeof(d->motion)); memset(d->confirmed_motion, 0, sizeof(d->confirmed_motion));
    memset(&d->trace_base, 0, sizeof(d->trace_base)); d->trace_fault = KMS_KEY_OK;
    memset(d->submitted, 0, sizeof(d->submitted));
}
void kms_delivery_select(kms_delivery_t *d, unsigned slot, uint32_t receiver, uint32_t now)
{
    kms_delivery_stop(d); ++d->generation;
    d->slot = slot; d->receiver = receiver; d->seen = d->snapshot = now;
    d->flags = KMS_FLAG_USB_READY; d->have_status = true;
    packet(d, KMS_ACTIVATE);
}
void kms_delivery_fault(kms_delivery_t *d, enum kms_event e, uint32_t now)
{
    if (!d->slot || (unsigned)e >= KMS_EVENT_COUNT) return;
    d->trace_fault = fault_reason(e);
    uint32_t bit = 1u << (unsigned)e;
    if (d->recovering) {
        if (!(d->reported_faults & bit)) { d->reported_faults |= bit; notice(d, e, d->count); }
        return;
    }
    d->recovering = d->draining = true; d->recovery_at = now;
    d->reported_faults = bit;
    notice(d, e, d->count);
}
static void discard(kms_delivery_t *d, uint32_t now, enum kms_key_trace_reason reason)
{
    if (d->suppressed) return;
    unsigned lost = d->count;
    trace_buffer(d, KMS_KEY_MISSING, reason, lost);
    d->head = d->count = 0; d->has_pending = d->restore = false;
    d->trace_base = d->current;
    memset(d->submitted, 0, sizeof(d->submitted));
    d->pending_count = 0;
    d->suppressed = d->need_activate = d->block = true;
    memset(&d->confirmed, 0, sizeof(d->confirmed));
    memcpy(d->confirmed_motion, d->motion, sizeof(d->motion));
    if (!d->recovering) kms_delivery_fault(d, KMS_EVENT_TX_OVERFLOW, now);
    notice(d, KMS_EVENT_REPLAY_DROPPED, lost);
}
void kms_delivery_abandon(kms_delivery_t *d, enum kms_event reason, uint32_t now)
{
    if (!d->slot) return;
    kms_delivery_fault(d, reason, now); discard(d, now, fault_reason(reason));
}
void kms_delivery_input(kms_delivery_t *d, const kms_state_t *s, int32_t x, int32_t y, int32_t wheel, int32_t pan, uint32_t now)
{
    bool changed = !kms_state_equal(&d->current, s);
    kms_state_t before = d->current;
    d->current = *s;
    if (!changed && !x && !y && !wheel && !pan) return;
    uint32_t id = ++d->next_trace_id;
    if (!id) id = ++d->next_trace_id;
    if (d->slot) {
        d->motion[0] += (uint32_t)x; d->motion[1] += (uint32_t)y;
        d->motion[2] += (uint32_t)wheel; d->motion[3] += (uint32_t)pan;
    }
    if (!d->slot || d->suppressed) {
        kms_key_trace_emit(d->trace, d->ctx, KMS_KEY_MISSING,
            d->slot ? KMS_KEY_SUPPRESSED : KMS_KEY_NO_ROUTE, 0, d->slot,
            id, d->generation, 0, &before, s);
        return;
    }
    // Only unsent pointer-only samples with identical held state are coalesced.
    if (!changed && d->count > (d->has_pending ? d->pending_count : 0)) {
        kms_buffered_t *tail = &d->buffer[(d->head + d->count - 1) % KMS_RECOVERY_BUFFER_DEPTH];
        if (kms_state_equal(&tail->state, s)) { memcpy(tail->motion, d->motion, sizeof(d->motion)); return; }
    }
    if (d->count == KMS_RECOVERY_BUFFER_DEPTH) {
        discard(d, now, KMS_KEY_BUFFER_FULL);
        kms_key_trace_emit(d->trace, d->ctx, KMS_KEY_MISSING, KMS_KEY_BUFFER_FULL,
            0, d->slot, id, d->generation, 0, &before, s);
        return;
    }
    if (!d->count) d->trace_base = before;
    kms_buffered_t *b = &d->buffer[(d->head + d->count++) % KMS_RECOVERY_BUFFER_DEPTH];
    b->state = *s; b->at = now; b->trace_id = id; memcpy(b->motion, d->motion, sizeof(d->motion));
    kms_key_trace_emit(d->trace, d->ctx, KMS_KEY_QUEUED, KMS_KEY_OK,
        0, d->slot, id, d->generation, 0, &before, s);
}
static void recovered(kms_delivery_t *d, uint32_t now)
{
    if (!d->recovering) return;
    notice(d, KMS_EVENT_RECOVERED, now - d->recovery_at);
    d->recovering = false;
    if (d->suppressed) {
        d->suppressed = false; d->block = true;
        memset(&d->current, 0, sizeof(d->current));
        memcpy(d->motion, d->confirmed_motion, sizeof(d->motion));
    }
}
static void confirm_input(kms_delivery_t *d)
{
    const kms_state_t *state = &d->pending.state;
    const uint32_t *motion = d->pending.motion;
    if (d->pending.type == KMS_INPUT_BATCH) {
        const kms_packet_input_t *last = &d->pending.extra[d->pending.batch_count - 2];
        state = &last->state; motion = last->motion;
    }
    d->confirmed = *state; memcpy(d->confirmed_motion, motion, sizeof(d->confirmed_motion));
}
static void acknowledge(kms_delivery_t *d, uint32_t now)
{
    d->ack_rtt_ms = now - d->first;
    if (d->ack_rtt_ms > d->ack_max_ms) d->ack_max_ms = d->ack_rtt_ms;
    ++d->ack_count;
    d->acknowledged_sequence = d->pending.sequence;
    if (d->pending.type == KMS_ACTIVATE)
        memcpy(d->confirmed_motion, d->pending.motion, sizeof(d->confirmed_motion));
    if (kms_packet_is_input(&d->pending)) {
        confirm_input(d);
        if (d->pending_count) {
            trace_buffer(d, KMS_KEY_CONFIRMED, KMS_KEY_OK, d->pending_count);
            retire_trace(d, d->pending_count);
            d->head = (d->head + d->pending_count) % KMS_RECOVERY_BUFFER_DEPTH;
            d->count -= d->pending_count;
        }
    }
    d->pending_count = 0;
    d->has_pending = d->started = false;
    // A batch already spreads its samples over a complete radio round trip.
    // Pace recovering batches between submissions; adding another full pause
    // after each ACK would keep their service rate below a continuing load.
    d->replay_at = d->pending.type == KMS_INPUT_BATCH ? d->sent : now;
    if (!d->need_activate) recovered(d, now);
}
static void activate(kms_delivery_t *d)
{
    ++d->generation; d->acknowledged_sequence = 0; packet(d, KMS_ACTIVATE);
    memcpy(d->pending.motion, d->confirmed_motion, sizeof(d->confirmed_motion));
    kms_state_t zero = {0}; d->restore = !kms_state_equal(&d->confirmed, &zero);
    d->need_activate = false;
}
void kms_delivery_receive(kms_delivery_t *d, const kms_packet_t *p, uint32_t now)
{
    if (p->type == KMS_ACK && d->releasing && d->release_started && p->slot == d->release.slot &&
        p->session == d->release.session && p->receiver == d->release.receiver &&
        p->generation == d->release.generation && p->sequence == d->release.sequence) {
        d->releasing = false; return;
    }
    if (!d->slot || p->slot != d->slot) return;
    if (p->type == KMS_ACK) {
        if (!d->has_pending || !d->started || p->session != d->boot || p->receiver != d->receiver ||
            p->generation != d->pending.generation || p->sequence != d->pending.sequence) return;
        // A matching input/activation ACK is fresh contact even if a periodic
        // status packet is delayed. Unmatched or duplicate ACKs do not count.
        d->seen = now;
        if (!(p->flags & KMS_FLAG_ACTIVE)) {
            kms_delivery_fault(d, KMS_EVENT_LEASE_EXPIRED, now); d->need_activate = true;
        }
        acknowledge(d, now); return;
    }
    if (p->type != KMS_STATUS) return;
    d->batch_limit = kms_batch_limit(p);
    bool restarted = d->receiver != p->session;
    d->seen = now; d->flags = p->flags; d->have_status = true;
    // A delayed status from before an acknowledged packet cannot turn a healthy
    // route into a new generation and replay an input that already executed.
    if (!restarted && p->receiver == d->boot &&
        (kms_newer(d->generation, p->generation) ||
         (p->generation == d->generation && d->acknowledged_sequence &&
          kms_newer(d->acknowledged_sequence, p->sequence)))) return;
    if (restarted) {
        kms_delivery_fault(d, KMS_EVENT_PEER_RESTART, now);
        // After a receiver restart its acceptance history is gone. Do not duplicate
        // any in-flight input whose delivery can no longer be established.
        if (d->has_pending && d->started && kms_packet_is_input(&d->pending) && d->pending_count) {
            trace_buffer(d, KMS_KEY_MISSING, KMS_KEY_RECEIVER_RESTART, d->pending_count);
            retire_trace(d, d->pending_count);
            confirm_input(d);
            d->head = (d->head + d->pending_count) % KMS_RECOVERY_BUFFER_DEPTH;
            d->count -= d->pending_count;
            notice(d, KMS_EVENT_REPLAY_DROPPED, d->pending_count);
        }
        d->pending_count = 0;
        memset(&d->confirmed, 0, sizeof(d->confirmed));
        d->receiver = p->session; d->has_pending = false; d->need_activate = true;
    }
    if (!(p->flags & KMS_FLAG_USB_READY)) {
        kms_delivery_fault(d, KMS_EVENT_USB_UNAVAILABLE, now); d->need_activate = true; return;
    }
    // Status is also an authenticated receipt for the exact in-flight sequence.
    // It resolves a lost ACK before rebuilding the generation after lease expiry.
    if (!restarted && d->has_pending && d->started && p->receiver == d->boot &&
        p->generation == d->pending.generation && p->sequence == d->pending.sequence) {
        if (!(p->flags & KMS_FLAG_ACTIVE)) d->need_activate = true;
        acknowledge(d, now);
    }
    bool ours = p->receiver == d->boot && p->generation == d->generation;
    if (ours && !(p->flags & KMS_FLAG_ACTIVE)) {
        kms_delivery_fault(d, KMS_EVENT_LEASE_EXPIRED, now); d->need_activate = true;
    }
    // Leave an activation in flight intact: a new master may have to wait for the
    // old session's lease, and regenerating its sequence would defeat deduplication.
    if (d->need_activate && (!d->has_pending || d->pending.type != KMS_ACTIVATE)) activate(d);
}
void kms_delivery_tick(kms_delivery_t *d, uint32_t now)
{
    if (d->releasing) {
        if (d->release_started && kms_time_elapsed(now, d->release_first, KMS_RADIO_RETRY_WINDOW_MS))
            d->releasing = false;
        else {
            if (!d->release_started || kms_time_elapsed(now, d->release_sent, KMS_RADIO_RETRY_MS)) {
                // The old route's release budget stays bounded even if the
                // driver cannot submit it. Its receiver lease is the fallback.
                d->retransmission = d->release_started;
                if (!d->release_started) { d->release_started = true; d->release_first = now; }
                if (d->send && d->send(d->ctx, &d->release)) d->release_sent = now;
            }
            return;
        }
    }
    if (!d->slot) return;
    if (kms_time_elapsed(now, d->seen, KMS_PEER_TIMEOUT_MS)) kms_delivery_fault(d, KMS_EVENT_PEER_LOST, now);
    if (!d->suppressed && ((d->count && kms_time_elapsed(now, d->buffer[d->head].at, KMS_RECOVERY_BUFFER_MS)) ||
        (d->recovering && kms_time_elapsed(now, d->recovery_at, KMS_RECOVERY_BUFFER_MS))))
        discard(d, now, d->trace_fault ? d->trace_fault :
            (d->has_pending && !d->pending_traced ? KMS_KEY_RADIO_BUSY : KMS_KEY_BUFFER_EXPIRED));
    if (d->need_activate && d->have_status && (d->flags & KMS_FLAG_USB_READY) &&
        !kms_time_elapsed(now, d->seen, KMS_PEER_TIMEOUT_MS) &&
        (!d->has_pending || d->pending.type != KMS_ACTIVATE)) activate(d);
    if (!d->has_pending && !d->need_activate && !d->suppressed) {
        if (d->draining && !kms_time_elapsed(now, d->replay_at, KMS_RECOVERY_REPLAY_MS)) return;
        if (d->restore) {
            packet(d, KMS_INPUT); d->pending.state = d->confirmed;
            memcpy(d->pending.motion, d->confirmed_motion, sizeof(d->confirmed_motion));
            d->pending_restore = true; d->restore = false;
        } else if (d->count) {
            kms_buffered_t *b = &d->buffer[d->head]; packet(d, KMS_INPUT);
            d->pending.state = b->state; memcpy(d->pending.motion, b->motion, sizeof(b->motion));
            d->pending_count = 1;
            unsigned batch = d->batch_limit < KMS_INPUT_BATCH_MAX ? d->batch_limit : KMS_INPUT_BATCH_MAX;
            if (batch > d->count) batch = d->count;
            if (batch >= 2) {
                d->pending.type = KMS_INPUT_BATCH; d->pending.batch_count = (uint8_t)batch;
                d->pending_count = batch;
                for (unsigned i = 1; i < batch; ++i) {
                    b = &d->buffer[(d->head + i) % KMS_RECOVERY_BUFFER_DEPTH];
                    d->pending.extra[i - 1].state = b->state;
                    memcpy(d->pending.extra[i - 1].motion, b->motion, sizeof(b->motion));
                }
                // Sparse key lists usually fit four inputs. Dense rollover
                // states use full bitmaps and reduce the batch to fit the frame.
                while (kms_batch_wire_size(&d->pending) > KMS_MAX_WIRE_SIZE) --d->pending.batch_count;
                d->pending_count = d->pending.batch_count;
            }
        } else if (d->recovering || kms_time_elapsed(now, d->snapshot, KMS_SNAPSHOT_MS)) {
            packet(d, KMS_INPUT); d->pending.state = d->current;
            memcpy(d->pending.motion, d->motion, sizeof(d->motion)); d->pending_restore = true;
        }
        if (!d->count) d->draining = false;
    }
    if (!d->has_pending) return;
    if (d->need_activate && !(d->flags & KMS_FLAG_USB_READY)) return;
    if (d->started && kms_time_elapsed(now, d->first, KMS_RADIO_RETRY_WINDOW_MS))
        kms_delivery_fault(d, KMS_EVENT_ACK_TIMEOUT, now);
    if (!d->recovering && !d->draining && d->count &&
        kms_time_elapsed(now, d->buffer[d->head].at, KMS_RADIO_MAX_AGE_MS))
        kms_delivery_fault(d, KMS_EVENT_ACK_TIMEOUT, now);
    if (!d->started || kms_time_elapsed(now, d->sent, KMS_RADIO_RETRY_MS)) {
        bool started = d->started;
        d->retransmission = started;
        if (!started) { d->started = true; d->first = now; }
        // A busy radio has not submitted this packet: do not start its ACK
        // deadline or consume a retry interval until the send is accepted.
        if (d->send && d->send(d->ctx, &d->pending)) {
            d->sent = d->snapshot = now;
            if (!d->pending_traced) {
                d->pending_traced = true;
                for (unsigned n = 0; n < d->pending_count; ++n) {
                    d->submitted[n].generation = d->pending.generation;
                    d->submitted[n].sequence = d->pending.sequence;
                }
                trace_buffer(d, KMS_KEY_SUBMITTED, KMS_KEY_OK, d->pending_count);
            }
        }
        else d->started = started;
    }
}
