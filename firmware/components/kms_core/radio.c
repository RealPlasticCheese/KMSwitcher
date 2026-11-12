#include "kms_radio.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8));
}
static uint32_t get32(const uint8_t *p)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= (uint32_t)p[i] << (i * 8);
    return n;
}
bool kms_radio_channel_valid(unsigned channel, unsigned first, unsigned count)
{
    return first >= 1 && first <= 13 && count >= 1 && count <= 14 - first && channel >= first && channel - first < count;
}
bool kms_radio_encode(const kms_radio_message_t *m, uint8_t wire[KMS_RADIO_BYTES])
{
    if (!m || !wire || m->type < KMS_RADIO_QUERY || m->type > KMS_RADIO_PROBE_REPLY ||
        !kms_radio_channel_valid(m->channel, 1, 13) || m->slot < 1 || m->slot > 7 ||
        !m->boot || !m->nonce ||
        (m->type == KMS_RADIO_PROBE_REPLY ?
            (m->flags & ~KMS_RADIO_PROBE_BUSY) || m->delay_ms >= KMS_RADIO_PROBE_TIMEOUT_MS :
            m->flags || m->delay_ms > (m->type == KMS_RADIO_REPLY ? KMS_RADIO_SWITCH_MS : 0))) return false;
    memset(wire, 0, KMS_RADIO_BYTES); memcpy(wire, "KMSC", 4);
    wire[4] = 1; wire[5] = m->type; wire[6] = m->channel; wire[7] = m->slot;
    put32(wire + 8, m->boot); put32(wire + 12, m->nonce);
    wire[16] = (uint8_t)m->delay_ms; wire[17] = (uint8_t)(m->delay_ms >> 8);
    wire[18] = m->flags;
    return true;
}
bool kms_radio_decode(kms_radio_message_t *m, const uint8_t *wire, size_t bytes)
{
    if (!m || !wire || bytes != KMS_RADIO_BYTES || memcmp(wire, "KMSC", 4) ||
        wire[4] != 1 || wire[19]) return false;
    *m = (kms_radio_message_t){.type = wire[5], .channel = wire[6], .slot = wire[7],
        .flags = wire[18], .boot = get32(wire + 8), .nonce = get32(wire + 12),
        .delay_ms = (uint16_t)(wire[16] | ((uint16_t)wire[17] << 8))};
    uint8_t check[KMS_RADIO_BYTES];
    return kms_radio_encode(m, check);
}
void kms_radio_search_init(kms_radio_search_t *s, unsigned channel, uint32_t now)
{
    *s = (kms_radio_search_t){.channel = (uint8_t)channel, .contact = now, .hopped = now};
}
void kms_radio_search_contact(kms_radio_search_t *s, uint32_t now)
{
    s->contact = s->hopped = now; s->searching = false;
}
unsigned kms_radio_search_tick(kms_radio_search_t *s, uint32_t now, unsigned first,
                               unsigned count, bool pairing, bool paired, bool freeze)
{
    if (freeze || (!pairing && (!paired || now - s->contact < KMS_RADIO_CONTACT_MS)) ||
        now - s->hopped < KMS_RADIO_DWELL_MS || !kms_radio_channel_valid(first, first, count)) return s->channel;
    s->searching = true; s->hopped = now;
    s->channel = (uint8_t)(kms_radio_channel_valid(s->channel + 1u, first, count) ? s->channel + 1u : first);
    return s->channel;
}
