#include "kms_stream.h"
#include <string.h>

bool kms_stream_profile(unsigned profile, kms_stream_profile_t *out)
{
    static const kms_stream_profile_t profiles[] = {{10, 1, 2}, {5, 6, 4}, {2, 12, 4}};
    if (!out || profile >= KMS_STREAM_PROFILE_COUNT) return false;
    *out = profiles[profile]; return true;
}
bool kms_stream_bulk(const kms_management_frame_t *f)
{
    if (!f || f->flags & KMS_M_SUMMARY) return false;
    if (f->op == KMS_M_CLIPBOARD && f->length == 192 && f->data[4] == 4) return true;
    return !(f->flags & KMS_M_REPLY) && (f->op == KMS_M_OTA_WRITE || f->op == KMS_M_CACHE_WRITE);
}
void kms_stream_pool_init(kms_stream_pool_t *p, kms_stream_slot_t *slots, unsigned capacity)
{
    memset(p, 0, sizeof(*p)); p->slots = slots; p->capacity = capacity;
    if (slots) memset(slots, 0, capacity * sizeof(*slots));
}
unsigned kms_stream_free(const kms_stream_pool_t *p)
{
    unsigned free = 0;
    if (p && p->slots) for (unsigned i = 0; i < p->capacity; ++i) free += !p->slots[i].used;
    return free;
}
bool kms_stream_submit(kms_stream_pool_t *p, const kms_management_frame_t *f, unsigned profile)
{
    if (!p || !p->slots || !f || f->length > KMS_M_DATA || profile >= KMS_STREAM_PROFILE_COUNT) return false;
    for (unsigned i = 0; i < p->capacity; ++i) if (!p->slots[i].used) {
        p->slots[i].frame = *f; p->slots[i].profile = (uint8_t)profile;
        p->slots[i].order = ++p->sequence; p->slots[i].used = true; return true;
    }
    return false;
}
unsigned kms_stream_peek(kms_stream_pool_t *p, bool bulk, unsigned *indices, unsigned maximum)
{
    unsigned count = 0;
    if (!p || !p->slots || !indices) return 0;
    if (maximum > KMS_STREAM_WINDOW) maximum = KMS_STREAM_WINDOW;
    for (unsigned n = 0; n < maximum; ++n) {
        unsigned best = p->capacity;
        for (unsigned i = 0; i < p->capacity; ++i) {
            kms_stream_slot_t *s = &p->slots[i];
            if (!s->used || kms_stream_bulk(&s->frame) != bulk) continue;
            bool selected = false;
            for (unsigned j = 0; j < count; ++j) selected |= indices[j] == i;
            if (selected) continue;
            if (count) {
                kms_stream_slot_t *first = &p->slots[indices[0]];
                if (s->frame.slot != first->frame.slot || s->frame.target != first->frame.target ||
                    s->frame.sender != first->frame.sender || s->profile != first->profile) continue;
            }
            if (best == p->capacity || (int32_t)(s->order - p->slots[best].order) < 0) best = i;
        }
        if (best == p->capacity) break;
        indices[count++] = best;
    }
    return count;
}
void kms_stream_release(kms_stream_pool_t *p, const unsigned *indices, unsigned count)
{ for (unsigned i = 0; p && i < count; ++i) if (indices[i] < p->capacity) p->slots[indices[i]].used = false; }
size_t kms_stream_encode(const kms_stream_batch_t *b, uint8_t *wire, size_t capacity)
{
    if (!b || !wire || capacity < KMS_STREAM_HEADER + 4 || !b->sender || !b->target || !b->sequence ||
        b->profile >= KMS_STREAM_PROFILE_COUNT || b->count > KMS_STREAM_WINDOW ||
        !b->slot || b->slot > 7 || b->credit > KMS_STREAM_WINDOW || b->flags > KMS_STREAM_HELLO_REPLY ||
        ((b->flags != 0) != (b->count == 0))) return 0;
    memset(wire, 0, KMS_STREAM_HEADER); memcpy(wire, "KMB2", 4);
    wire[4] = KMS_STREAM_VERSION; wire[5] = b->flags; wire[6] = b->count; wire[7] = b->profile;
    kms_put_u32(wire + 8, b->sender); kms_put_u32(wire + 12, b->target); kms_put_u32(wire + 16, b->sequence);
    wire[20] = b->credit; wire[21] = b->slot;
    size_t used = KMS_STREAM_HEADER;
    for (unsigned i = 0; i < b->count; ++i) {
        if (capacity < used + 2 + 4) return 0;
        size_t bytes = kms_management_encode(&b->frames[i], wire + used + 2, capacity - used - 2 - 4);
        if (!bytes || b->frames[i].slot != b->slot || b->frames[i].sender != b->sender || b->frames[i].target != b->target || !kms_stream_bulk(&b->frames[i])) return 0;
        wire[used] = (uint8_t)bytes; wire[used + 1] = (uint8_t)(bytes >> 8); used += 2 + bytes;
    }
    wire[22] = (uint8_t)(used - KMS_STREAM_HEADER); wire[23] = (uint8_t)((used - KMS_STREAM_HEADER) >> 8);
    if (used + 4 > KMS_STREAM_MAX_WIRE || used + 4 > capacity) return 0;
    kms_put_u32(wire + used, kms_crc32(wire, used)); return used + 4;
}
bool kms_stream_decode(kms_stream_batch_t *b, const uint8_t *wire, size_t length)
{
    if (!b || !wire || length < KMS_STREAM_HEADER + 4 || length > KMS_STREAM_MAX_WIRE ||
        memcmp(wire, "KMB2", 4) || wire[4] != KMS_STREAM_VERSION ||
        (unsigned)(wire[22] | wire[23] << 8) != length - KMS_STREAM_HEADER - 4 ||
        kms_crc32(wire, length - 4) != kms_get_u32(wire + length - 4)) return false;
    memset(b, 0, sizeof(*b)); b->flags = wire[5]; b->count = wire[6]; b->profile = wire[7];
    b->sender = kms_get_u32(wire + 8); b->target = kms_get_u32(wire + 12);
    b->sequence = kms_get_u32(wire + 16); b->credit = wire[20]; b->slot = wire[21];
    if (!b->sender || !b->target || !b->sequence || b->count > KMS_STREAM_WINDOW || b->credit > KMS_STREAM_WINDOW ||
        !b->slot || b->slot > 7 || b->profile >= KMS_STREAM_PROFILE_COUNT || b->flags > KMS_STREAM_HELLO_REPLY ||
        ((b->flags != 0) != (b->count == 0))) return false;
    size_t at = KMS_STREAM_HEADER;
    for (unsigned i = 0; i < b->count; ++i) {
        if (at + 2 > length - 4) return false;
        unsigned size = wire[at] | wire[at + 1] << 8; at += 2;
        if (size > length - 4 - at || !kms_management_decode(&b->frames[i], wire + at, size) ||
            b->frames[i].slot != b->slot || b->frames[i].sender != b->sender || b->frames[i].target != b->target || !kms_stream_bulk(&b->frames[i])) return false;
        at += size;
    }
    return at == length - 4;
}
bool kms_stream_cursor_init(kms_stream_cursor_t *c, uint32_t size, uint32_t offset, unsigned block)
{ if (!c || offset > size || !block) return false; *c = (kms_stream_cursor_t){size, offset, block}; return true; }
unsigned kms_stream_cursor_next(const kms_stream_cursor_t *c)
{ uint32_t left = c->size - c->offset; return left < c->block ? left : c->block; }
bool kms_stream_cursor_commit(kms_stream_cursor_t *c, uint32_t offset)
{ if (offset != c->offset + kms_stream_cursor_next(c)) return false; c->offset = offset; return true; }
