#pragma once
#include "kms_management.h"

enum { KMS_STREAM_VERSION = 1, KMS_STREAM_SLOTS = 8, KMS_STREAM_WINDOW = 4,
       KMS_STREAM_HEADER = 24, KMS_STREAM_MAX_WIRE = 940,
       KMS_STREAM_HELLO = 1, KMS_STREAM_HELLO_REPLY = 2, KMS_STREAM_CAPABILITY = 64,
       KMS_STREAM_STATUS_TAG = 0x53000000, KMS_STREAM_PROFILE_COUNT = 3 };
/* Explicit values: profile numbers are persisted by clients. */
enum { KMS_STREAM_PROFILE_CONSERVATIVE = 0, KMS_STREAM_PROFILE_BALANCED = 1, KMS_STREAM_PROFILE_FAST = 2 };
typedef struct { uint16_t pace_ms, rate_mbps; uint8_t window; } kms_stream_profile_t;
bool kms_stream_profile(unsigned, kms_stream_profile_t *);
bool kms_stream_bulk(const kms_management_frame_t *);
typedef struct {
    kms_management_frame_t frame;
    uint8_t profile;
    bool used;
    uint32_t order;
} kms_stream_slot_t;
typedef struct { kms_stream_slot_t *slots; uint32_t sequence; unsigned capacity; } kms_stream_pool_t;
void kms_stream_pool_init(kms_stream_pool_t *, kms_stream_slot_t *, unsigned);
bool kms_stream_submit(kms_stream_pool_t *, const kms_management_frame_t *, unsigned);
/* Peek leaves ownership in the pool until the driver accepts the transmission. */
unsigned kms_stream_peek(kms_stream_pool_t *, bool bulk, unsigned *indices, unsigned maximum);
void kms_stream_release(kms_stream_pool_t *, const unsigned *, unsigned);
unsigned kms_stream_free(const kms_stream_pool_t *);
typedef struct {
    uint32_t sender, target, sequence;
    uint8_t flags, profile, count, credit, slot;
    kms_management_frame_t frames[KMS_STREAM_WINDOW];
} kms_stream_batch_t;
size_t kms_stream_encode(const kms_stream_batch_t *, uint8_t *, size_t);
bool kms_stream_decode(kms_stream_batch_t *, const uint8_t *, size_t);

/* Source/sink-independent committed-offset cursor, used by cached OTA as well. */
typedef struct { uint32_t size, offset; unsigned block; } kms_stream_cursor_t;
bool kms_stream_cursor_init(kms_stream_cursor_t *, uint32_t size, uint32_t offset, unsigned block);
unsigned kms_stream_cursor_next(const kms_stream_cursor_t *);
bool kms_stream_cursor_commit(kms_stream_cursor_t *, uint32_t offset);
