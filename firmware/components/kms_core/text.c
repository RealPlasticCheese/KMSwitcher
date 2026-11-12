#include "kms_text.h"
#include "kms_log_buffer.h"
#include <string.h>

static char character(const kms_single_packet_t *p, unsigned index)
{ return (char)(index < 32 ? p->state.keys[index] : p->public_key[index - 32]); }
bool kms_text_packet(kms_packet_t *p, unsigned operation, const char *text, size_t length)
{
    if (operation < KMS_TEXT_QUERY || operation > KMS_TEXT_CANCEL || length > KMS_TEXT_BYTES ||
        (operation == KMS_TEXT_DATA ? (!text || !length) : length != 0)) return false;
    p->type = KMS_TEXT; p->flags = (uint8_t)operation; p->motion[0] = (uint32_t)length;
    memset(&p->state, 0, sizeof(p->state)); memset(p->public_key, 0, sizeof(p->public_key));
    for (unsigned n = 0; n < length; ++n) {
        if (n < 32) p->state.keys[n] = (uint8_t)text[n];
        else p->public_key[n - 32] = (uint8_t)text[n];
    }
    return true;
}
bool kms_text_valid(const kms_packet_t *p)
{
    return (p->type == KMS_TEXT || p->type == KMS_TEXT_ACK) && p->slot && p->slot <= KMS_SLOTS &&
        p->session && p->receiver && p->generation && p->sequence &&
        p->flags >= KMS_TEXT_QUERY && p->flags <= KMS_TEXT_CANCEL &&
        (p->flags == KMS_TEXT_DATA ? p->motion[0] > 0 && p->motion[0] <= KMS_TEXT_BYTES : !p->motion[0]);
}
void kms_text_ack(const kms_single_packet_t *p, kms_single_packet_t *out)
{ *out = *p; out->type = KMS_TEXT_ACK; }
void kms_text_tx_start(kms_text_tx_t *t, const kms_packet_t *p, uint32_t now)
{
    t->packet = *p; t->first = now; t->pending = true; t->submitted = t->failed = false;
}
void kms_text_tx_tick(kms_text_tx_t *t, uint32_t now, kms_text_send_fn send, void *ctx)
{
    if (!t->pending) return;
    bool probe = t->packet.flags == KMS_TEXT_QUERY;
    if (kms_time_elapsed(now, t->first, probe ? KMS_TEXT_PROBE_MS : KMS_TEXT_TIMEOUT_MS)) {
        t->pending = false;
        if (probe) t->supported = false;
        else t->failed = true;
        return;
    }
    if ((!t->submitted || kms_time_elapsed(now, t->sent, KMS_TEXT_RETRY_MS)) && send(ctx, &t->packet)) {
        t->submitted = true; t->sent = now;
    }
}
bool kms_text_tx_receive(kms_text_tx_t *t, const kms_packet_t *p)
{
    const kms_packet_t *q = &t->packet;
    if (!t->pending || !t->submitted || p->type != KMS_TEXT_ACK || !kms_text_valid(p) ||
        p->slot != q->slot || p->session != q->session || p->receiver != q->receiver ||
        p->generation != q->generation || p->sequence != q->sequence || p->flags != q->flags ||
        p->motion[0] != q->motion[0]) return false;
    t->pending = false;
    if (q->flags == KMS_TEXT_QUERY) t->supported = true;
    return true;
}
bool kms_text_rx_receive(kms_text_rx_t *r, const kms_packet_t *p,
                         kms_receiver_guard_t *g, uint32_t now, bool *cancel)
{
    *cancel = false;
    if (p->type != KMS_TEXT || !kms_text_valid(p) || !g->active ||
        p->session != g->session || p->generation != g->generation) return false;
    bool same_route = r->initialized && p->session == r->packet.session && p->generation == r->packet.generation;
    if (same_route && p->sequence == r->packet.sequence) {
        if (p->flags != r->packet.flags || p->motion[0] != r->packet.motion[0] ||
            memcmp(p->state.keys, r->packet.state.keys, 32) || memcmp(p->public_key, r->packet.public_key, 32)) return false;
        g->last_seen = now;
        return r->complete; // Lost ACKs never replay already typed characters.
    }
    if (same_route && (!kms_newer(p->sequence, r->packet.sequence) ||
        (r->active && p->flags != KMS_TEXT_CANCEL))) return false;
    *cancel = p->flags == KMS_TEXT_CANCEL;
    *r = (kms_text_rx_t){.packet = p->single, .initialized = true,
        .active = p->flags == KMS_TEXT_DATA, .complete = p->flags != KMS_TEXT_DATA};
    g->last_seen = now;
    return r->complete;
}
bool kms_text_rx_next(kms_text_rx_t *r, bool caps, kms_sample_t *out)
{
    if (!r->active) return false;
    *out = (kms_sample_t){0};
    if (r->release) return true;
    uint8_t key, modifiers;
    while (r->offset < r->packet.motion[0]) {
        if (kms_log_key(character(&r->packet, r->offset), caps, &key, &modifiers)) {
            kms_set_key(&out->state, key, true); out->state.modifiers = modifiers; return true;
        }
        ++r->offset;
    }
    r->active = false; r->complete = true; return false;
}
void kms_text_rx_delivered(kms_text_rx_t *r)
{
    if (!r->active) return;
    if (!r->release) { r->release = true; return; }
    r->release = false;
    if (++r->offset == r->packet.motion[0]) { r->active = false; r->complete = true; }
}
