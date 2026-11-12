#include "kms_clipboard.h"
#include <string.h>

bool kms_clipboard_encode(const kms_clipboard_packet_t *p, uint8_t *b)
{
    if (!p || !b || p->kind < KMS_C_QUERY || p->kind > KMS_C_ERROR ||
        !p->source || p->source > KMS_SLOTS || !p->target || p->target > KMS_SLOTS ||
        p->source == p->target || !p->source_session || !p->target_session ||
        !p->sequence || p->length > KMS_C_DATA) return false;
    memset(b, 0, KMS_C_BYTES); memcpy(b, "KCB1", 4);
    b[4] = p->kind; b[5] = p->source; b[6] = p->target; b[7] = p->length;
    kms_put_u32(b + 8, p->source_session); kms_put_u32(b + 12, p->target_session);
    kms_put_u32(b + 16, p->sequence); kms_put_u32(b + 20, p->transfer); kms_put_u32(b + 24, p->offset);
    memcpy(b + 28, p->data, p->length); kms_put_u32(b + 188, kms_crc32(b, 188)); return true;
}
bool kms_clipboard_decode(kms_clipboard_packet_t *p, const uint8_t *b, size_t n)
{
    if (!p || !b || n != KMS_C_BYTES || memcmp(b, "KCB1", 4) ||
        kms_get_u32(b + 188) != kms_crc32(b, 188)) return false;
    kms_clipboard_packet_t v = {.kind = b[4], .source = b[5], .target = b[6], .length = b[7],
        .source_session = kms_get_u32(b + 8), .target_session = kms_get_u32(b + 12),
        .sequence = kms_get_u32(b + 16), .transfer = kms_get_u32(b + 20), .offset = kms_get_u32(b + 24)};
    if (v.length > KMS_C_DATA) return false;
    memcpy(v.data, b + 28, v.length);
    uint8_t check[KMS_C_BYTES];
    if (!kms_clipboard_encode(&v, check) || memcmp(check, b, KMS_C_BYTES)) return false;
    *p = v; return true;
}
bool kms_clipboard_authorized(const kms_desktop_master_t *m, const kms_clipboard_packet_t *p,
                              unsigned source, uint32_t boot, uint32_t now)
{
    if (!m || !p || !source || source > KMS_SLOTS || p->source != source ||
        !p->target || p->target > KMS_SLOTS || p->target == source) return false;
    const kms_desktop_peer_t *a = &m->peers[source - 1], *b = &m->peers[p->target - 1];
    return a->boot == boot && a->session && b->session &&
        a->session == p->source_session && b->session == p->target_session &&
        !kms_time_elapsed(now, a->seen, KMS_D_LEASE_MS) && !kms_time_elapsed(now, b->seen, KMS_D_LEASE_MS);
}
