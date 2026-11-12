#pragma once
#include "kms_core.h"

// Separate packet types keep old receivers compatible: an unanswered probe
// falls back to the original acknowledged-keyboard path before any text is sent.
enum { KMS_TEXT_BYTES = 64, KMS_TEXT_QUERY = 1, KMS_TEXT_DATA, KMS_TEXT_CANCEL,
       KMS_TEXT_RETRY_MS = 40, KMS_TEXT_PROBE_MS = 160, KMS_TEXT_TIMEOUT_MS = 2000 };
typedef bool (*kms_text_send_fn)(void *, const kms_packet_t *);
typedef struct {
    kms_packet_t packet;
    uint32_t first, sent;
    bool pending, submitted, supported, failed;
} kms_text_tx_t;
typedef struct {
    kms_single_packet_t packet;
    unsigned offset;
    bool initialized, active, release, complete;
} kms_text_rx_t;

bool kms_text_packet(kms_packet_t *, unsigned operation, const char *, size_t);
bool kms_text_valid(const kms_packet_t *);
void kms_text_tx_start(kms_text_tx_t *, const kms_packet_t *, uint32_t now);
void kms_text_tx_tick(kms_text_tx_t *, uint32_t now, kms_text_send_fn, void *);
bool kms_text_tx_receive(kms_text_tx_t *, const kms_packet_t *);
// The caller verifies the paired source MAC and local receiver boot first.
// True requests an ACK; cancel requests clearing queued/held USB output.
bool kms_text_rx_receive(kms_text_rx_t *, const kms_packet_t *,
                         kms_receiver_guard_t *, uint32_t now, bool *cancel);
bool kms_text_rx_next(kms_text_rx_t *, bool caps_lock, kms_sample_t *);
// Advance only after the USB report has actually completed, including key-up.
void kms_text_rx_delivered(kms_text_rx_t *);
void kms_text_ack(const kms_single_packet_t *, kms_single_packet_t *);
