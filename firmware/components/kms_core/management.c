#include "kms_management.h"
#include "kms_settings.h"
#include <string.h>

uint32_t kms_crc32_extend(uint32_t previous, const void *bytes, size_t length)
{
    const uint8_t *p = bytes;
    uint32_t crc = ~previous;
    while (length--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
uint32_t kms_crc32(const void *bytes, size_t length)
{ return kms_crc32_extend(0, bytes, length); }
void kms_put_u32(uint8_t *p, uint32_t n)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8)); }
uint32_t kms_get_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

unsigned kms_restart_request(kms_restart_t *state, const kms_management_frame_t *q, uint32_t boot, bool busy, uint32_t now)
{
    if (!state || !q || q->op != KMS_M_RESTART || !q->id || !q->sender || q->flags || q->status ||
        q->offset || (q->length != 4 && q->length != 8) || kms_get_u32(q->data) != KMS_RESTART_KEY ||
        (q->length == 8 && kms_get_u32(q->data + 4) != KMS_RESTART_AUTOMATIC)) return KMS_M_BAD_REQUEST;
    if (!q->target || q->target != boot) return KMS_M_WRONG_BOOT;
    if (state->pending) return state->sender == q->sender && state->id == q->id ? KMS_M_OK : KMS_M_BUSY;
    if (busy) return KMS_M_BUSY;
    *state = (kms_restart_t){.pending = true, .requested_at = now, .sender = q->sender, .id = q->id};
    return KMS_M_OK;
}
bool kms_restart_take(kms_restart_t *state, uint32_t now)
{
    if (!state->pending || (int32_t)(now - state->requested_at) < KMS_RESTART_DELAY_MS) return false;
    state->pending = false; return true;
}

bool kms_management_changes_firmware(unsigned op)
{
    return (op >= KMS_M_OTA_BEGIN && op <= KMS_M_OTA_ABORT) ||
        (op >= KMS_M_CACHE_BEGIN && op <= KMS_M_CACHE_INSTALL);
}
bool kms_remote_upload_busy(const kms_remote_upload_t *upload, uint32_t peer_boot, uint32_t now)
{
    return upload->token && (!peer_boot || upload->boot == peer_boot) &&
        (uint32_t)(now - upload->at) < KMS_MANAGEMENT_SESSION_MS;
}
void kms_remote_upload_reply(kms_remote_upload_t *upload, const kms_management_frame_t *q,
                             unsigned result, uint32_t now)
{
    if (q->op < KMS_M_OTA_BEGIN || q->op > KMS_M_OTA_ABORT || q->length < 4 || !q->target) return;
    uint32_t token = kms_get_u32(q->data);
    if (!token) return;
    bool matching = upload->boot == q->target && upload->token == token;
    if ((matching && (result == KMS_M_EXPIRED || result == KMS_M_WRONG_BOOT ||
                     (q->op == KMS_M_OTA_END && result == KMS_M_INTEGRITY) ||
                     (q->op == KMS_M_OTA_WRITE && result == KMS_M_STORAGE))) ||
        (q->op == KMS_M_OTA_BEGIN && result == KMS_M_CURRENT) ||
        (matching && result == KMS_M_OK && (q->op == KMS_M_OTA_ACTIVATE || q->op == KMS_M_OTA_ABORT))) {
        upload->token = 0;
    } else if (matching && result == KMS_M_TIMEOUT) {
        upload->at = now;
    } else if (q->op <= KMS_M_OTA_END && (result == KMS_M_OK || result == KMS_M_TIMEOUT)) {
        // A missing reply can still mean flash is active. Keep its normal
        // inactivity lease; only a matching terminal reply or new boot ends it.
        if (result == KMS_M_TIMEOUT && kms_remote_upload_busy(upload, q->target, now) && !matching)
            upload->at = now;
        else *upload = (kms_remote_upload_t){.boot = q->target, .token = token, .at = now};
    }
}

size_t kms_management_encode(const kms_management_frame_t *f, uint8_t *out, size_t cap)
{
    if (!f || !out || f->length > KMS_M_DATA || cap < KMS_M_HEADER + f->length + 4u ||
        !f->id || !f->slot || f->slot > 7 || f->op < KMS_M_IDENTITY || f->op > KMS_M_STREAM_STATUS ||
        (f->flags & ~(KMS_M_REPLY | KMS_M_SUMMARY | KMS_M_FORCE))) return 0;
    memset(out, 0, KMS_M_HEADER);
    memcpy(out, "KMSM", 4); out[4] = KMS_M_VERSION; out[5] = f->op;
    out[6] = f->flags; out[7] = f->status; out[8] = f->slot;
    kms_put_u32(out + 12, f->id); kms_put_u32(out + 16, f->sender);
    kms_put_u32(out + 20, f->target); kms_put_u32(out + 24, f->offset);
    out[28] = (uint8_t)f->length; out[29] = (uint8_t)(f->length >> 8);
    memcpy(out + KMS_M_HEADER, f->data, f->length);
    size_t length = KMS_M_HEADER + f->length;
    kms_put_u32(out + length, kms_crc32(out, length));
    return length + 4;
}
bool kms_management_decode(kms_management_frame_t *f, const uint8_t *in, size_t length)
{
    if (!f || !in || length < KMS_M_HEADER + 4 || length > KMS_M_MAX_WIRE ||
        memcmp(in, "KMSM", 4) || in[4] != KMS_M_VERSION ||
        in[5] < KMS_M_IDENTITY || in[5] > KMS_M_STREAM_STATUS ||
        (in[6] & ~(KMS_M_REPLY | KMS_M_SUMMARY | KMS_M_FORCE)) ||
        !in[8] || in[8] > 7 || in[9] || in[10] || in[11] ||
        in[7] > KMS_M_TIMEOUT || kms_get_u32(in + 12) == 0) return false;
    unsigned bytes = in[28] | (unsigned)in[29] << 8;
    if (bytes > KMS_M_DATA || length != KMS_M_HEADER + bytes + 4u ||
        kms_get_u32(in + length - 4) != kms_crc32(in, length - 4)) return false;
    memset(f, 0, sizeof(*f));
    f->op = in[5]; f->flags = in[6]; f->status = in[7]; f->slot = in[8];
    f->id = kms_get_u32(in + 12); f->sender = kms_get_u32(in + 16);
    f->target = kms_get_u32(in + 20); f->offset = kms_get_u32(in + 24);
    f->length = (uint16_t)bytes; memcpy(f->data, in + KMS_M_HEADER, bytes);
    return true;
}
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool kms_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *length)
{
    size_t count = 0;
    while (*hex) {
        int a = hex_digit(*hex++);
        if (!*hex || a < 0 || count == cap) return false;
        int b = hex_digit(*hex++);
        if (b < 0) return false;
        out[count++] = (uint8_t)((a << 4) | b);
    }
    *length = count; return true;
}
void kms_hex_encode(const uint8_t *bytes, size_t length, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        out[i * 2] = digits[bytes[i] >> 4]; out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[length * 2] = 0;
}
const char *kms_management_result_name(unsigned result)
{
    static const char *const names[] = {"ok", "bad request", "busy", "receiver restarted",
        "unexpected offset", "integrity failure", "storage failure", "unsupported",
        "already current", "expired session", "incompatible image", "timeout/unreachable"};
    return result <= KMS_M_TIMEOUT ? names[result] : "unknown";
}
void kms_identity_encode(const kms_firmware_identity_t *i, uint8_t *out)
{
    memcpy(out, i->git, 48); memcpy(out + 48, i->utc, 24); memcpy(out + 72, i->sha256, 32);
    kms_put_u32(out + 104, i->image_bytes); kms_put_u32(out + 108, i->chip);
    kms_put_u32(out + 112, i->capabilities); kms_put_u32(out + 116, KMS_M_VERSION);
}
bool kms_identity_decode(kms_firmware_identity_t *i, const uint8_t *in, size_t length)
{
    bool git_terminated = false, utc_terminated = false;
    if (length != KMS_IDENTITY_BYTES) return false;
    for (unsigned n = 0; n < 48; ++n) if (!in[n]) git_terminated = true;
    for (unsigned n = 48; n < 72; ++n) if (!in[n]) utc_terminated = true;
    if (length != KMS_IDENTITY_BYTES || kms_get_u32(in + 116) != KMS_M_VERSION ||
        !git_terminated || !utc_terminated) return false;
    memcpy(i->git, in, 48); memcpy(i->utc, in + 48, 24); memcpy(i->sha256, in + 72, 32);
    i->image_bytes = kms_get_u32(in + 104); i->chip = kms_get_u32(in + 108);
    i->capabilities = kms_get_u32(in + 112); return true;
}

void kms_upload_init(kms_upload_t *u, const kms_upload_io_t *io)
{ memset(u, 0, sizeof(*u)); u->io = *io; }
void kms_upload_abort(kms_upload_t *u)
{
    if (u->active && !u->verified) u->io.abort(u->io.ctx);
    u->active = u->verified = u->activated = false; u->token = u->offset = 0;
}
unsigned kms_upload_begin(kms_upload_t *u, uint32_t token, uint32_t size,
                          const uint8_t *hash, bool force, const uint8_t *running, uint32_t capacity)
{
    if (!token || !size || size > capacity) return KMS_M_INCOMPATIBLE;
    if (u->active) {
        if (u->token == token && u->size == size && !memcmp(u->sha256, hash, 32)) return KMS_M_OK;
        return KMS_M_BUSY;
    }
    if (!force && !memcmp(hash, running, 32)) return KMS_M_CURRENT;
    if (!u->io.begin(u->io.ctx, size)) return KMS_M_STORAGE;
    u->token = token; u->size = size; u->offset = u->last_size = 0;
    memcpy(u->sha256, hash, 32); u->active = true; u->verified = u->activated = false;
    return KMS_M_OK;
}
unsigned kms_upload_write(kms_upload_t *u, uint32_t token, uint32_t offset, const uint8_t *data, size_t size)
{
    if (!u->active || token != u->token) return KMS_M_EXPIRED;
    if (u->verified || !size || size > KMS_M_DATA - 4) return KMS_M_BAD_REQUEST;
    if (offset == u->last_offset && size == u->last_size && kms_crc32(data, size) == u->last_crc &&
        !memcmp(data, u->last_data, size))
        return KMS_M_OK;
    if (offset != u->offset || offset > u->size || size > u->size - offset) return KMS_M_OFFSET;
    if (!u->io.write(u->io.ctx, offset, data, size)) { kms_upload_abort(u); return KMS_M_STORAGE; }
    u->last_offset = offset; u->last_size = (uint32_t)size; u->last_crc = kms_crc32(data, size);
    memcpy(u->last_data, data, size);
    u->offset += (uint32_t)size; return KMS_M_OK;
}
unsigned kms_upload_end(kms_upload_t *u, uint32_t token)
{
    if (!u->active || token != u->token) return KMS_M_EXPIRED;
    if (u->verified) return KMS_M_OK;
    if (u->offset != u->size) return KMS_M_OFFSET;
    if (!u->io.verify(u->io.ctx, u->size, u->sha256)) { kms_upload_abort(u); return KMS_M_INTEGRITY; }
    u->verified = true; return KMS_M_OK;
}
unsigned kms_upload_activate(kms_upload_t *u, uint32_t token)
{
    if (!u->active || token != u->token) return KMS_M_EXPIRED;
    if (!u->verified) return KMS_M_INTEGRITY;
    if (u->activated) return KMS_M_OK;
    if (!u->io.activate(u->io.ctx)) return KMS_M_STORAGE;
    u->activated = true; return KMS_M_OK;
}
unsigned kms_boot_health_decide(uint32_t start, uint32_t now, bool app, bool usb, bool radio,
                                uint32_t grace, uint32_t deadline)
{
    uint32_t elapsed = now - start;
    if (elapsed >= deadline) return KMS_BOOT_ROLLBACK;
    return elapsed >= grace && app && usb && radio ? KMS_BOOT_CONFIRM : KMS_BOOT_WAIT;
}
