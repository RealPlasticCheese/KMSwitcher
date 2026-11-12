#pragma once
#include "kms_stream.h"

enum { KMS_CACHED_IDLE, KMS_CACHED_IDENTITY, KMS_CACHED_BEGIN, KMS_CACHED_WRITE,
       KMS_CACHED_VERIFY, KMS_CACHED_ACTIVATE, KMS_CACHED_CONFIRM,
       KMS_CACHED_UPDATED, KMS_CACHED_SKIPPED, KMS_CACHED_FAILED };

typedef struct {
    unsigned (*exchange)(void *, kms_management_frame_t *, kms_management_frame_t *);
    bool (*read)(void *, uint32_t, void *, size_t);
    void *ctx;
    unsigned (*exchange_window)(void *, kms_management_frame_t *, unsigned);
} kms_cached_update_io_t;

typedef struct {
    kms_cached_update_io_t io;
    kms_firmware_identity_t image;
    uint32_t token, sequence, boot, offset, next_at, confirm_at;
    unsigned state, result, slot, begin_attempts;
    uint8_t previous_hash[32];
    bool force;
    kms_stream_cursor_t stream;
} kms_cached_update_t;

bool kms_cached_update_busy(const kms_cached_update_t *);
void kms_cached_update_start(kms_cached_update_t *, const kms_cached_update_io_t *,
                             const kms_firmware_identity_t *, unsigned, bool, uint32_t);
void kms_cached_update_step(kms_cached_update_t *, uint32_t);
