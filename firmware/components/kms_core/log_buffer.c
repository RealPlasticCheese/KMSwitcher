#include "kms_log_buffer.h"
#include <string.h>

void kms_log_buffer_expire(kms_log_buffer_t *b, uint32_t now)
{
    while (b->first < b->end &&
           (uint32_t)(now - b->records[b->first % b->capacity].at) >= KMS_LOG_KEEP_MS) ++b->first;
}
void kms_log_buffer_append(kms_log_buffer_t *b, uint32_t now, const char *text, size_t length)
{
    if (!b->capacity || !length) return;
    kms_log_buffer_expire(b, now);
    if (b->end - b->first == b->capacity) { ++b->first; ++b->evicted; }
    kms_log_record_t *r = &b->records[b->end++ % b->capacity];
    r->at = now;
    bool truncated = length > sizeof(r->text);
    if (truncated) { length = sizeof(r->text); ++b->truncated; }
    r->length = (uint16_t)length;
    memcpy(r->text, text, length);
    if (truncated) {
        static const char marker[] = " [truncated]\n";
        memcpy(r->text + length - (sizeof(marker) - 1), marker, sizeof(marker) - 1);
    }
}
bool kms_log_buffer_read(const kms_log_buffer_t *b, uint64_t cursor, kms_log_record_t *out)
{
    if (cursor < b->first || cursor >= b->end) return false;
    *out = b->records[cursor % b->capacity]; return true;
}
bool kms_log_key(char c, bool caps, uint8_t *usage, uint8_t *modifiers)
{
    *modifiers = 0;
    if (c >= 'a' && c <= 'z') { *usage = (uint8_t)(4 + c - 'a'); *modifiers = caps ? 2 : 0; return true; }
    if (c >= 'A' && c <= 'Z') { *usage = (uint8_t)(4 + c - 'A'); *modifiers = caps ? 0 : 2; return true; }
    if (c >= '1' && c <= '9') { *usage = (uint8_t)(30 + c - '1'); return true; }
    if (c == '0') { *usage = 39; return true; }
    if (c == '\n') { *usage = 40; return true; }
    if (c == '\b') { *usage = 42; return true; }
    if (c == '\t' || c == ' ') { *usage = 44; return true; }
    if (c == '\r') return false;
    static const char plain[] = "-=[]\\;'`,./";
    static const char shifted[] = "_+{}|:\"~<>?";
    static const uint8_t codes[] = {45,46,47,48,49,51,52,53,54,55,56};
    for (unsigned i = 0; i < sizeof(codes); ++i) {
        if (c == plain[i] || c == shifted[i]) {
            *usage = codes[i]; *modifiers = c == shifted[i] ? 2 : 0; return true;
        }
    }
    static const char digits[] = "!@#$%^&*()";
    for (unsigned i = 0; i < 10; ++i) if (c == digits[i]) {
        *usage = (uint8_t)(30 + i); *modifiers = 2; return true;
    }
    *usage = 56; *modifiers = 2; return true;
}
