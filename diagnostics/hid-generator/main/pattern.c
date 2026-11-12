#include "pattern.h"
#include <string.h>
static void put(uint8_t *p, uint32_t v, unsigned n)
{ for (unsigned i = 0; i < n; ++i) p[i] = (uint8_t)(v >> (i * 8)); }
void kms_generator_pattern(uint8_t *out, unsigned source, uint32_t run,
                           uint32_t sequence, uint32_t seed, unsigned profile)
{
    memset(out, 0, KMS_GEN_BYTES);
    if (sequence) {
        unsigned first = source == 1 ? 4 : 20;
        if (profile == 3) {
            if (sequence & 1u) out[1] = 1u << 4; // Both sources share A.
        } else if (profile == 1) {
            for (unsigned bit = 0; bit < 16; ++bit) if (sequence & (1u << bit))
                out[1 + (first + bit) / 8] |= (uint8_t)(1u << ((first + bit) % 8));
        } else if ((sequence & 1u) && (profile == 0 || profile == 4 ||
                   (profile == KMS_GEN_MIXED && source == 1 && sequence % 64 < 16))) {
            unsigned key = first + ((sequence / 2) % 16);
            out[1 + key / 8] |= (uint8_t)(1u << (key % 8));
            if ((sequence / 2) % 7 == 0) out[0] = source == 1 ? 2 : 32;
        }
        if (profile != KMS_GEN_MIXED || source == 2) {
            uint32_t value = sequence * 1664525u + seed * 1013904223u;
            int32_t distance = (int32_t)((value >> 16) % 31u) + 1;
            if ((sequence / 32) & 1u) distance = -distance;
            put(out + 33, ((sequence / 4) & 1u) ? (profile == 3 ? 1u : (1u << (source - 1))) : 0, 2);
            put(out + (source == 1 ? 35 : 37), (uint32_t)distance, 2);
            out[source == 1 ? 39 : 40] = (uint8_t)((sequence & 1u) ? 1 : -1);
        }
        if (profile == 4 && (sequence & 1u)) put(out + 41, source == 1 ? 0xe9 : 0xea, 2);
    }
    out[43] = 'K'; out[44] = 'M'; out[45] = (uint8_t)source;
    put(out + 46, run, 4); put(out + 50, sequence, 4);
}
