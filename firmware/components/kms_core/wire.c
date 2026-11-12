#include "kms_core.h"
#include <string.h>

_Static_assert(KMS_BATCH_HEADER_BYTES + KMS_INPUT_BATCH_MAX * (1 + KMS_BATCH_INPUT_TAIL_BYTES) <=
               KMS_MAX_WIRE_SIZE, "Minimum-size negotiated batches must fit one radio frame");

bool kms_newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
bool kms_time_elapsed(uint32_t now, uint32_t since, uint32_t duration)
{ return (int32_t)(now - since) >= (int32_t)duration; }
bool kms_state_equal(const kms_state_t *a, const kms_state_t *b)
{
    return a->modifiers == b->modifiers && a->buttons == b->buttons && a->system == b->system &&
           !memcmp(a->keys, b->keys, KMS_KEY_BYTES) && !memcmp(a->consumer, b->consumer, sizeof(a->consumer));
}
void kms_state_merge(kms_state_t *a, const kms_state_t *b)
{
    a->modifiers |= b->modifiers; a->buttons |= b->buttons; a->system |= b->system;
    for (unsigned i = 0; i < KMS_KEY_BYTES; ++i) a->keys[i] |= b->keys[i];
    for (unsigned i = 0; i < KMS_CONSUMERS; ++i) if (b->consumer[i]) {
        for (unsigned j = 0; j < KMS_CONSUMERS; ++j) {
            if (a->consumer[j] == b->consumer[i]) break;
            if (!a->consumer[j]) { a->consumer[j] = b->consumer[i]; break; }
        }
    }
}
static void put(uint8_t **p, uint32_t v, unsigned n)
{ for (unsigned i = 0; i < n; ++i) *(*p)++ = (uint8_t)(v >> (8 * i)); }
static uint32_t get(const uint8_t **p, unsigned n)
{ uint32_t v = 0; for (unsigned i = 0; i < n; ++i) v |= (uint32_t)*(*p)++ << (8 * i); return v; }
static void put_input_tail(uint8_t **p, const kms_state_t *state, const uint32_t motion[4])
{
    put(p, state->modifiers, 1); put(p, state->buttons, 2);
    for (unsigned i = 0; i < KMS_CONSUMERS; ++i) put(p, state->consumer[i], 2);
    put(p, state->system, 1);
    for (unsigned i = 0; i < 4; ++i) put(p, motion[i], 4);
}
static void get_input_tail(const uint8_t **p, kms_state_t *state, uint32_t motion[4])
{
    state->modifiers = (uint8_t)get(p, 1); state->buttons = (uint16_t)get(p, 2);
    for (unsigned i = 0; i < KMS_CONSUMERS; ++i) state->consumer[i] = (uint16_t)get(p, 2);
    state->system = (uint8_t)get(p, 1);
    for (unsigned i = 0; i < 4; ++i) motion[i] = get(p, 4);
}
static void put_input(uint8_t **p, const kms_state_t *state, const uint32_t motion[4])
{
    memcpy(*p, state->keys, KMS_KEY_BYTES); *p += KMS_KEY_BYTES;
    put_input_tail(p, state, motion);
}
static void get_input(const uint8_t **p, kms_state_t *state, uint32_t motion[4])
{
    memcpy(state->keys, *p, KMS_KEY_BYTES); *p += KMS_KEY_BYTES;
    get_input_tail(p, state, motion);
}
static unsigned key_bytes(const kms_state_t *state)
{
    unsigned count = 0;
    for (unsigned i = 0; i < KMS_KEY_BYTES; ++i) {
        uint8_t bits = state->keys[i];
        while (bits) {
            if (++count == KMS_KEY_BYTES) return KMS_KEY_BYTES;
            bits &= (uint8_t)(bits - 1);
        }
    }
    return count;
}
static void put_compact_input(uint8_t **p, const kms_state_t *state, const uint32_t motion[4])
{
    unsigned keys = key_bytes(state); put(p, keys, 1);
    if (keys == KMS_KEY_BYTES) { memcpy(*p, state->keys, KMS_KEY_BYTES); *p += KMS_KEY_BYTES; }
    else if (keys) for (unsigned usage = 0; usage < 256; ++usage)
        if (kms_key(state, usage)) put(p, usage, 1);
    put_input_tail(p, state, motion);
}
static bool get_compact_input(const uint8_t **p, const uint8_t *end, kms_state_t *state, uint32_t motion[4])
{
    if (*p == end) return false;
    unsigned keys = *(*p)++;
    if (keys > KMS_KEY_BYTES || (size_t)(end - *p) < keys + KMS_BATCH_INPUT_TAIL_BYTES) return false;
    memset(state->keys, 0, sizeof(state->keys));
    if (keys == KMS_KEY_BYTES) { memcpy(state->keys, *p, keys); *p += keys; }
    else for (unsigned i = 0, previous = 0; i < keys; ++i) {
        unsigned usage = *(*p)++;
        if (i && usage <= previous) return false;
        kms_set_key(state, usage, true); previous = usage;
    }
    get_input_tail(p, state, motion); return true;
}
size_t kms_batch_wire_size(const kms_packet_t *s)
{
    if (!s || s->batch_count < 2 || s->batch_count > KMS_INPUT_BATCH_MAX) return 0;
    size_t bytes = KMS_BATCH_HEADER_BYTES + 1 + key_bytes(&s->state) + KMS_BATCH_INPUT_TAIL_BYTES;
    for (unsigned i = 1; i < s->batch_count; ++i)
        bytes += 1 + key_bytes(&s->extra[i - 1].state) + KMS_BATCH_INPUT_TAIL_BYTES;
    return bytes;
}
void kms_batch_advertise(kms_packet_t *status)
{
    memcpy(status->public_key, "KMSB", 4);
    status->public_key[4] = KMS_BATCH_VERSION; status->public_key[5] = KMS_INPUT_BATCH_MAX;
}
unsigned kms_batch_limit(const kms_packet_t *status)
{
    unsigned count = status->public_key[5];
    return status->type == KMS_STATUS && !memcmp(status->public_key, "KMSB", 4) &&
        status->public_key[4] == KMS_BATCH_VERSION && count >= 2 && count <= KMS_INPUT_BATCH_MAX ? count : 1;
}
static size_t batch_encode(const kms_packet_t *s, uint8_t *out, size_t capacity)
{
    unsigned count = s->batch_count;
    size_t bytes = kms_batch_wire_size(s);
    if (!bytes || bytes > KMS_MAX_WIRE_SIZE || !s->slot || s->slot > KMS_SLOTS ||
        s->flags || !s->sequence || capacity < bytes) return 0;
    uint8_t *p = out;
    memcpy(p, "KMSB", 4); p += 4;
    put(&p, KMS_BATCH_VERSION, 1); put(&p, count, 1); put(&p, s->slot, 1); put(&p, 0, 1);
    put(&p, s->session, 4); put(&p, s->receiver, 4); put(&p, s->generation, 4); put(&p, s->sequence, 4);
    put_compact_input(&p, &s->state, s->motion);
    for (unsigned i = 1; i < count; ++i) put_compact_input(&p, &s->extra[i - 1].state, s->extra[i - 1].motion);
    return (size_t)(p - out);
}
static bool batch_decode(kms_packet_t *s, const uint8_t *in, size_t len)
{
    if (len < KMS_BATCH_HEADER_BYTES || len > KMS_MAX_WIRE_SIZE || in[4] != KMS_BATCH_VERSION || in[5] < 2 ||
        in[5] > KMS_INPUT_BATCH_MAX || !in[6] || in[6] > KMS_SLOTS || in[7]) return false;
    memset(s, 0, sizeof(*s)); s->type = KMS_INPUT_BATCH; s->batch_count = in[5]; s->slot = in[6];
    const uint8_t *p = in + 8;
    s->session = get(&p, 4); s->receiver = get(&p, 4); s->generation = get(&p, 4); s->sequence = get(&p, 4);
    if (!s->sequence) return false;
    const uint8_t *end = in + len;
    if (!get_compact_input(&p, end, &s->state, s->motion)) return false;
    for (unsigned i = 1; i < s->batch_count; ++i)
        if (!get_compact_input(&p, end, &s->extra[i - 1].state, s->extra[i - 1].motion)) return false;
    return p == end;
}
size_t kms_wire_encode(const kms_packet_t *s, uint8_t *out, size_t capacity)
{
    if (s && out && s->type == KMS_INPUT_BATCH) return batch_encode(s, out, capacity);
    if (!s || !out || capacity < KMS_WIRE_SIZE) return 0;
    uint8_t *p = out;
    memcpy(p, "KMSW", 4); p += 4;
    put(&p, KMS_WIRE_VERSION, 1); put(&p, s->type, 1); put(&p, s->slot, 1); put(&p, s->flags, 1);
    put(&p, s->session, 4); put(&p, s->receiver, 4); put(&p, s->generation, 4); put(&p, s->sequence, 4);
    put_input(&p, &s->state, s->motion);
    memcpy(p, s->public_key, 32); p += 32;
    memcpy(p, s->nonce, 16); p += 16;
    put(&p, (s->keyboard_leds.leds & KMS_LOCK_MASK) | (s->keyboard_leds.valid ? KMS_LOCK_VALID : 0), 1);
    put(&p, s->keyboard_leds.sequence, 4);
    return (size_t)(p - out);
}
bool kms_wire_decode(kms_packet_t *s, const uint8_t *in, size_t len)
{
    if (s && in && len >= 4 && !memcmp(in, "KMSB", 4)) return batch_decode(s, in, len);
    if (!s || !in || len != KMS_WIRE_SIZE || memcmp(in, "KMSW", 4) || in[4] != KMS_WIRE_VERSION ||
        in[5] < KMS_STATUS || in[5] > KMS_TEXT_ACK || in[6] > KMS_SLOTS ||
        (in[KMS_WIRE_SIZE - 5] & ~(KMS_LOCK_MASK | KMS_LOCK_VALID))) return false;
    memset(s, 0, sizeof(*s));
    const uint8_t *p = in + 5;
    s->type = (uint8_t)get(&p, 1); s->slot = (uint8_t)get(&p, 1); s->flags = (uint8_t)get(&p, 1);
    s->session = get(&p, 4); s->receiver = get(&p, 4); s->generation = get(&p, 4); s->sequence = get(&p, 4);
    get_input(&p, &s->state, s->motion);
    memcpy(s->public_key, p, 32); p += 32; memcpy(s->nonce, p, 16); p += 16;
    uint8_t leds = (uint8_t)get(&p, 1);
    s->keyboard_leds.leds = leds & KMS_LOCK_MASK; s->keyboard_leds.valid = (leds & KMS_LOCK_VALID) != 0;
    s->keyboard_leds.sequence = get(&p, 4);
    return true;
}
bool kms_receiver_accept(kms_receiver_guard_t *g, const kms_packet_t *p, uint32_t now,
                         kms_sample_t *out, bool *deliver)
{
    *deliver = false;
    if (p->type != KMS_ACTIVATE && p->type != KMS_INPUT && p->type != KMS_RELEASE) return false;
    if (p->type == KMS_ACTIVATE) {
        if (g->initialized && p->session == g->session && p->generation == g->generation) {
            if (p->sequence == g->sequence) return true;
            return false;
        }
        if (g->initialized && p->session == g->session && !kms_newer(p->generation, g->generation)) return false;
        if (g->active && p->session != g->session) return false;
        g->session = p->session; g->generation = p->generation; g->sequence = p->sequence;
        memcpy(g->motion, p->motion, sizeof(g->motion));
        g->initialized = g->active = true; g->last_seen = now;
        memset(out, 0, sizeof(*out)); *deliver = true; return true;
    }
    if (!g->initialized || p->session != g->session || p->generation != g->generation) return false;
    if (p->sequence == g->sequence) return true;
    if (!kms_newer(p->sequence, g->sequence) || (!g->active && p->type != KMS_RELEASE)) return false;
    memset(out, 0, sizeof(*out));
    if (p->type == KMS_INPUT) {
        out->state = p->state;
        out->x = (int32_t)(p->motion[0] - g->motion[0]); out->y = (int32_t)(p->motion[1] - g->motion[1]);
        out->wheel = (int32_t)(p->motion[2] - g->motion[2]); out->pan = (int32_t)(p->motion[3] - g->motion[3]);
        memcpy(g->motion, p->motion, sizeof(g->motion));
    } else g->active = false;
    g->sequence = p->sequence; g->last_seen = now; *deliver = true;
    return true;
}
bool kms_receiver_accept_many(kms_receiver_guard_t *g, const kms_packet_t *p, uint32_t now,
                              kms_sample_t out[KMS_INPUT_BATCH_MAX], unsigned *count, bool *apply)
{
    *count = 0;
    if (p->type != KMS_INPUT_BATCH) {
        bool accepted = kms_receiver_accept(g, p, now, out, apply);
        if (accepted && *apply) *count = 1;
        return accepted;
    }
    *apply = false;
    if (p->batch_count < 2 || p->batch_count > KMS_INPUT_BATCH_MAX || p->flags || !p->sequence) return false;
    kms_packet_t first = *p; first.type = KMS_INPUT;
    if (!kms_receiver_accept(g, &first, now, out, apply)) return false;
    if (!*apply) return true;
    *count = p->batch_count;
    for (unsigned i = 1; i < *count; ++i) {
        const kms_packet_input_t *input = &p->extra[i - 1];
        out[i] = (kms_sample_t){.state = input->state,
            .x = (int32_t)(input->motion[0] - g->motion[0]),
            .y = (int32_t)(input->motion[1] - g->motion[1]),
            .wheel = (int32_t)(input->motion[2] - g->motion[2]),
            .pan = (int32_t)(input->motion[3] - g->motion[3])};
        memcpy(g->motion, input->motion, sizeof(g->motion));
    }
    return true;
}
bool kms_receiver_expire(kms_receiver_guard_t *g, uint32_t now)
{
    if (g->active && kms_time_elapsed(now, g->last_seen, KMS_LEASE_MS)) {
        g->active = false; return true;
    }
    return false;
}
