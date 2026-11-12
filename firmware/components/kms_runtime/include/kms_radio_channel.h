#pragma once
#include "kms_radio.h"

typedef bool (*kms_radio_send_fn)(const uint8_t *mac, const uint8_t *wire, size_t bytes);
void kms_radio_channel_init(bool master, uint32_t boot, kms_radio_send_fn send, void (*release)(void));
void kms_radio_channel_forget(unsigned slot);
void kms_radio_channel_peer(unsigned slot, const uint8_t *mac, const uint8_t *key);
bool kms_radio_channel_receive(const uint8_t *mac, const uint8_t *wire, size_t bytes,
                               uint32_t received_at, kms_radio_message_t *probe_reply);
void kms_radio_channel_tick(bool pairing, bool candidate, bool transmitter_busy);
void kms_radio_channel_probe_reply_tick(bool pairing);
void kms_radio_channel_processing_fault(uint32_t now);
bool kms_radio_channel_probe(unsigned slot, uint32_t boot, uint32_t nonce);
void kms_radio_channel_paired(void);
// Validated legacy HID/management traffic also proves the master is reachable.
void kms_radio_channel_contact(void);
unsigned kms_radio_channel_get(void);
bool kms_radio_channel_changing(void);
bool kms_radio_channel_recovering(void);
bool kms_radio_channel_switch_pending(void);
bool kms_radio_channel_trial_active(void);
bool kms_radio_channel_trial_available(void);
bool kms_radio_channel_trial_switch(unsigned channel);
void kms_radio_channel_trial_cancel_switch(void);
bool kms_radio_channel_trial_commit(void);
void kms_radio_channel_trial_end(void);
// Returns NULL on success, otherwise a stable console/UI error code.
const char *kms_radio_channel_set(unsigned channel);
void kms_radio_channel_status(unsigned long id, const char *result);
typedef struct {
    unsigned current, pending, first, count, paired, supported;
} kms_radio_channel_status_t;
void kms_radio_channel_snapshot(kms_radio_channel_status_t *out);
