#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t run, seed, profile, source, count, crc;
    uint32_t sequence_errors, payload_errors, stale, releases;
} iso_stream_t;

typedef struct { uint8_t interface, endpoint; uint16_t packet; } iso_endpoint_t;
bool iso_find_endpoint(const uint8_t *data, size_t bytes, iso_endpoint_t *out);
bool iso_serial(const uint8_t *data, size_t bytes, char out[13]);
void iso_stream_report(iso_stream_t *stream, const uint8_t *data, size_t bytes);
bool iso_elapsed(uint32_t now, uint32_t since, uint32_t limit);
