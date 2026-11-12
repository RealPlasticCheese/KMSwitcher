#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { KMS_LOG_TEXT_BYTES = 506, KMS_LOG_KEEP_MS = 3600000 };
typedef struct { uint32_t at; uint16_t length; char text[KMS_LOG_TEXT_BYTES]; } kms_log_record_t;
_Static_assert(sizeof(kms_log_record_t) == 512, "Log record capacity assumes 512 bytes");
typedef struct {
    kms_log_record_t *records;
    size_t capacity;
    uint64_t first, end, evicted, truncated;
} kms_log_buffer_t;
void kms_log_buffer_expire(kms_log_buffer_t *, uint32_t now);
void kms_log_buffer_append(kms_log_buffer_t *, uint32_t now, const char *, size_t length);
// A fixed [cursor,end) snapshot never follows newly arriving messages.
bool kms_log_buffer_read(const kms_log_buffer_t *, uint64_t cursor, kms_log_record_t *);
// US keyboard layout; unsupported bytes become '?', CR is skipped by the caller.
bool kms_log_key(char c, bool caps_lock, uint8_t *usage, uint8_t *modifiers);
