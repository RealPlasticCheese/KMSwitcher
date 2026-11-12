#include "stream.h"
#include "pattern.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool iso_elapsed(uint32_t now, uint32_t since, uint32_t limit)
{ return (uint32_t)(now - since) >= limit; }

bool iso_serial(const uint8_t *data, size_t bytes, char out[13])
{
    if (!data || bytes != 24) return false;
    for (unsigned n = 0; n < 12; ++n) {
        unsigned c = data[n * 2] | data[n * 2 + 1] << 8;
        if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
        out[n] = (char)c;
    }
    out[12] = 0; return true;
}

bool iso_find_endpoint(const uint8_t *data, size_t bytes, iso_endpoint_t *out)
{
    bool hid = false, found = false; uint8_t number = 0;
    for (size_t at = 0; at < bytes;) {
        if (bytes - at < 2 || data[at] < 2 || data[at] > bytes - at) return false;
        const uint8_t *d = data + at;
        if (d[1] == 4) {
            if (d[0] < 9) return false;
            hid = d[5] == 3 && d[3] == 0 && d[4] == 1; number = d[2];
        } else if (d[1] == 5 && hid) {
            if (d[0] < 7 || found || !(d[2] & 0x80) || (d[3] & 3) != 3) return false;
            uint16_t packet = (uint16_t)(d[4] | d[5] << 8);
            if (packet < KMS_GEN_BYTES + 1 || packet > 64 || !d[6]) return false;
            *out = (iso_endpoint_t){number, d[2], packet}; found = true;
        }
        at += d[0];
    }
    return found;
}

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t bytes)
{
    crc = ~crc;
    while (bytes--) {
        crc ^= *p++;
        for (unsigned n = 0; n < 8; ++n) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}

void iso_stream_report(iso_stream_t *s, const uint8_t *data, size_t bytes)
{
    if (bytes != KMS_GEN_BYTES + 1 || data[0] != 1 || data[44] != 'K' || data[45] != 'M' ||
        data[46] < 1 || data[46] > 2) { ++s->payload_errors; return; }
    const uint8_t *p = data + 1;
    if (get32(p + 46) != s->run) { ++s->stale; return; }
    if (!s->source) s->source = p[45];
    if (s->source != p[45]) { ++s->payload_errors; return; }
    uint32_t sequence = get32(p + 50);
    uint8_t expected[KMS_GEN_BYTES];
    kms_generator_pattern(expected, s->source, s->run, sequence, s->seed, s->profile);
    if (memcmp(expected, p, sizeof(expected))) ++s->payload_errors;
    if (!sequence) { ++s->releases; return; }
    if (sequence != s->count + 1) ++s->sequence_errors;
    ++s->count; s->crc = crc32(s->crc, p, KMS_GEN_BYTES);
}
