#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { KMS_RADIO_HEADER = 20, KMS_RADIO_TAG = 16, KMS_RADIO_BYTES = 36,
       KMS_RADIO_QUERY = 1, KMS_RADIO_REPLY = 2, KMS_RADIO_PROBE = 3, KMS_RADIO_PROBE_REPLY = 4,
       KMS_RADIO_PROBE_BUSY = 1, KMS_RADIO_PROBE_TIMEOUT_MS = 450,
       KMS_RADIO_CONTACT_MS = 4000, KMS_RADIO_DWELL_MS = 400,
       KMS_RADIO_QUERY_MS = 150, KMS_RADIO_HEARTBEAT_MS = 1000, KMS_RADIO_SWITCH_MS = 1800 };
typedef struct {
    uint8_t type, channel, slot, flags;
    uint32_t boot, nonce;
    uint16_t delay_ms; // Discovery switch delay or probe reply turnaround.
} kms_radio_message_t;
typedef struct { uint32_t contact, hopped; uint8_t channel; bool searching; } kms_radio_search_t;
bool kms_radio_channel_valid(unsigned channel, unsigned first, unsigned count);
bool kms_radio_encode(const kms_radio_message_t *, uint8_t wire[KMS_RADIO_BYTES]);
bool kms_radio_decode(kms_radio_message_t *, const uint8_t *wire, size_t bytes);
void kms_radio_search_init(kms_radio_search_t *, unsigned channel, uint32_t now);
void kms_radio_search_contact(kms_radio_search_t *, uint32_t now);
// A paired receiver searches only after contact times out. Pairing searches
// immediately; freeze keeps a key exchange or firmware update on its channel.
unsigned kms_radio_search_tick(kms_radio_search_t *, uint32_t now, unsigned first,
                               unsigned count, bool pairing, bool paired, bool freeze);
