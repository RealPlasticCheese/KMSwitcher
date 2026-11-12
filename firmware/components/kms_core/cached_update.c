#include "kms_cached_update.h"
#include <string.h>

bool kms_cached_update_busy(const kms_cached_update_t *update)
{ return update->state >= KMS_CACHED_IDENTITY && update->state <= KMS_CACHED_CONFIRM; }

void kms_cached_update_start(kms_cached_update_t *update, const kms_cached_update_io_t *io,
                             const kms_firmware_identity_t *image, unsigned slot, bool force, uint32_t token)
{
    memset(update, 0, sizeof(*update));
    update->io = *io;
    update->image = *image;
    update->slot = slot;
    update->token = update->sequence = token;
    update->force = force;
    update->state = KMS_CACHED_IDENTITY;
}

static unsigned exchange(kms_cached_update_t *u, unsigned op, kms_management_frame_t *q,
                          kms_management_frame_t *r)
{
    q->op = (uint8_t)op;
    q->slot = (uint8_t)u->slot;
    if (!++u->sequence) ++u->sequence;
    q->id = u->sequence;
    q->target = op == KMS_M_IDENTITY ? 0 : u->boot;
    q->flags = u->force ? KMS_M_FORCE : 0;
    return u->io.exchange(u->io.ctx, q, r);
}

static void fail(kms_cached_update_t *u, unsigned result)
{
    if (u->state >= KMS_CACHED_BEGIN && u->state < KMS_CACHED_CONFIRM) {
        kms_management_frame_t q = {.length = 4}, r = {0};
        kms_put_u32(q.data, u->token);
        (void)exchange(u, KMS_M_OTA_ABORT, &q, &r);
    }
    u->result = result;
    u->state = KMS_CACHED_FAILED;
}

void kms_cached_update_step(kms_cached_update_t *u, uint32_t now)
{
    if (!kms_cached_update_busy(u) || (u->next_at && (int32_t)(now - u->next_at) < 0)) return;
    kms_management_frame_t q = {0}, r = {0};
    unsigned result;
    if (u->state == KMS_CACHED_IDENTITY || u->state == KMS_CACHED_CONFIRM) {
        bool confirming = u->state == KMS_CACHED_CONFIRM;
        if (confirming && now - u->confirm_at >= 60000u) { fail(u, KMS_M_TIMEOUT); return; }
        result = exchange(u, KMS_M_IDENTITY, &q, &r);
        if (confirming && (result == KMS_M_TIMEOUT || result == KMS_M_WRONG_BOOT)) { u->next_at = now + 500; return; }
        if (result != KMS_M_OK) { fail(u, result); return; }
        kms_firmware_identity_t identity;
        if (!kms_identity_decode(&identity, r.data, r.length) || identity.chip != 2 || !(identity.capabilities & 2)) {
            fail(u, KMS_M_INCOMPATIBLE); return;
        }
        bool matching = !memcmp(identity.sha256, u->image.sha256, 32);
        if (confirming) {
            if (r.sender != u->boot && matching && (identity.capabilities & 4)) {
                u->state = KMS_CACHED_UPDATED; return;
            }
            if (r.sender != u->boot && !matching && !memcmp(identity.sha256, u->previous_hash, 32)) {
                fail(u, KMS_M_INTEGRITY); return;
            }
            u->next_at = now + 500; return;
        }
        if (matching && !u->force) { u->state = KMS_CACHED_SKIPPED; return; }
        if (!(identity.capabilities & 4)) { fail(u, KMS_M_BUSY); return; }
        u->boot = r.sender;
        memcpy(u->previous_hash, identity.sha256, 32);
        u->state = KMS_CACHED_BEGIN;
    } else if (u->state == KMS_CACHED_BEGIN) {
        q.length = 44;
        kms_put_u32(q.data, u->token); kms_put_u32(q.data + 4, u->image.image_bytes);
        memcpy(q.data + 8, u->image.sha256, 32); kms_put_u32(q.data + 40, 2);
        result = exchange(u, KMS_M_OTA_BEGIN, &q, &r);
        if (result == KMS_M_BUSY && ++u->begin_attempts < 8) { u->next_at = now + 500; return; }
        if (result == KMS_M_CURRENT) { u->state = KMS_CACHED_SKIPPED; return; }
        if (result != KMS_M_OK) { fail(u, result); return; }
        if (r.length != 4 || kms_get_u32(r.data)) { fail(u, KMS_M_OFFSET); return; }
        (void)kms_stream_cursor_init(&u->stream, u->image.image_bytes, 0, KMS_M_DATA - 4);
        u->state = KMS_CACHED_WRITE;
    } else if (u->state == KMS_CACHED_WRITE) {
        kms_management_frame_t frames[KMS_STREAM_WINDOW];
        kms_stream_cursor_t pending = u->stream;
        unsigned count = 0, maximum = u->io.exchange_window ? KMS_STREAM_WINDOW : 1;
        while (count < maximum && kms_stream_cursor_next(&pending)) {
            kms_management_frame_t *frame = &frames[count++]; memset(frame, 0, sizeof(*frame));
            unsigned bytes = kms_stream_cursor_next(&pending);
            frame->length = (uint16_t)(bytes + 4); frame->offset = pending.offset;
            frame->op = KMS_M_OTA_WRITE; frame->slot = (uint8_t)u->slot; frame->target = u->boot;
            if (!++u->sequence) ++u->sequence;
            frame->id = u->sequence; kms_put_u32(frame->data, u->token);
            if (!u->io.read(u->io.ctx, pending.offset, frame->data + 4, bytes)) { fail(u, KMS_M_STORAGE); return; }
            (void)kms_stream_cursor_commit(&pending, pending.offset + bytes);
        }
        if (u->io.exchange_window) result = u->io.exchange_window(u->io.ctx, frames, count);
        else {
            q = frames[0]; result = u->io.exchange(u->io.ctx, &q, &r);
            if (result == KMS_M_OK && (r.length != 4 || kms_get_u32(r.data) != pending.offset)) result = KMS_M_OFFSET;
        }
        if (result != KMS_M_OK) { fail(u, result); return; }
        u->stream = pending; u->offset = pending.offset;
        if (u->offset == u->image.image_bytes) u->state = KMS_CACHED_VERIFY;
    } else {
        q.length = 4; kms_put_u32(q.data, u->token);
        unsigned op = u->state == KMS_CACHED_VERIFY ? KMS_M_OTA_END : KMS_M_OTA_ACTIVATE;
        result = exchange(u, op, &q, &r);
        if (u->state == KMS_CACHED_ACTIVATE && (result == KMS_M_TIMEOUT || result == KMS_M_WRONG_BOOT)) result = KMS_M_OK;
        if (result != KMS_M_OK) { fail(u, result); return; }
        if (u->state == KMS_CACHED_VERIFY) u->state = KMS_CACHED_ACTIVATE;
        else { u->state = KMS_CACHED_CONFIRM; u->confirm_at = now; u->next_at = now + 500; }
    }
}
