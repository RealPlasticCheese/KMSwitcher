#include "kms_radio_channel.h"
#include "kms_runtime.h"
#include "kms_ota.h"
#include "kms_remote.h"
#include "kms_test_mode.h"
#include "pair_crypto.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

typedef struct {
    bool valid, pending;
    uint8_t mac[6], key[KMS_LINK_KEY_BYTES];
    kms_radio_message_t reply;
} channel_peer_t;
static channel_peer_t peers[KMS_SLOTS];
static uint8_t capable[KMS_SLOTS][6];
enum { CHANNEL_RECONNECT_MS = 12000, RECEIVER_PROCESSING_QUIET_MS = 1500 };
static bool master, changing, storage_ok, was_pairing, trial, manual_grace;
static unsigned current, saved, requested, next_reply;
static uint32_t boot_id, nonce, query_at, switch_at, manual_retuned_at;
static unsigned first_channel, channel_count;
static kms_radio_search_t search;
static kms_radio_message_t pending_probe;
static uint32_t probe_received_at, processing_fault_at;
static bool processing_fault_seen;
enum { SWITCH_HELD = 1, RECONNECT_HELD = 2 };
static portMUX_TYPE interlock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t interlock_state;
static kms_radio_send_fn send_frame;
static void (*release_input)(void);
static const char *TAG = "radio-channel";

static void publish_interlock(void)
{
    uint8_t state = ((changing || trial) ? SWITCH_HELD : 0) | (manual_grace ? RECONNECT_HELD : 0);
    portENTER_CRITICAL(&interlock);
    interlock_state = state;
    portEXIT_CRITICAL(&interlock);
}
static uint8_t read_interlock(void)
{
    portENTER_CRITICAL(&interlock);
    uint8_t state = interlock_state;
    portEXIT_CRITICAL(&interlock);
    return state;
}
static bool store(const char *name, const void *data, size_t bytes)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return false;
    e = nvs_set_blob(h, name, data, bytes);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "Cannot save %s: %s", name, esp_err_to_name(e));
    return e == ESP_OK;
}
static bool save_channel(unsigned channel)
{
    uint8_t value = (uint8_t)channel;
    if (channel == saved) return true;
    if (!store("radio_channel", &value, sizeof(value))) return false;
    saved = channel; return true;
}
static bool retune(unsigned channel)
{
    if (channel == current) return true;
    esp_err_t e = esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
    if (e != ESP_OK) { ESP_LOGW(TAG, "Channel %u failed: %s", channel, esp_err_to_name(e)); return false; }
    current = channel; search.channel = (uint8_t)channel; nonce = 0;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) peers[i].pending = false;
    pending_probe.nonce = 0;
    return true;
}
static bool tag(const uint8_t *key, const uint8_t *wire, uint8_t out[KMS_RADIO_TAG])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0; uint8_t digest[32]; size_t bytes = 0;
    bool ok = psa_import_key(&a, key, KMS_LINK_KEY_BYTES, &id) == PSA_SUCCESS &&
        psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), wire, KMS_RADIO_HEADER,
                        digest, sizeof(digest), &bytes) == PSA_SUCCESS && bytes == sizeof(digest);
    if (ok) memcpy(out, digest, KMS_RADIO_TAG);
    if (id) psa_destroy_key(id);
    psa_reset_key_attributes(&a); memset(digest, 0, sizeof(digest));
    return ok;
}
static bool transmit(channel_peer_t *p, const kms_radio_message_t *message)
{
    uint8_t wire[KMS_RADIO_BYTES];
    return kms_radio_encode(message, wire) && tag(p->key, wire, wire + KMS_RADIO_HEADER) &&
        send_frame(p->mac, wire, sizeof(wire));
}
void kms_radio_channel_init(bool is_master, uint32_t boot, kms_radio_send_fn send, void (*release)(void))
{
    memset(peers, 0, sizeof(peers)); memset(capable, 0, sizeof(capable));
    changing = was_pairing = trial = manual_grace = processing_fault_seen = false;
    publish_interlock();
    nonce = query_at = next_reply = pending_probe.nonce = 0;
    master = is_master; boot_id = boot; send_frame = send; release_input = release;
    wifi_country_t country;
    ESP_ERROR_CHECK(esp_wifi_get_country(&country));
    first_channel = country.schan; channel_count = country.nchan;
    current = KMS_RADIO_CHANNEL; saved = 0; storage_ok = true;
    nvs_handle_t h;
    esp_err_t e = nvs_open(KMS_NVS_NAMESPACE, NVS_READONLY, &h);
    if (e == ESP_OK) {
        uint8_t value = 0; size_t bytes = sizeof(value);
        e = nvs_get_blob(h, "radio_channel", &value, &bytes);
        if (e == ESP_OK && bytes == sizeof(value) && kms_radio_channel_valid(value, first_channel, channel_count))
            current = saved = value;
        else if (e != ESP_ERR_NVS_NOT_FOUND) storage_ok = false;
        bytes = sizeof(capable);
        if (nvs_get_blob(h, "radio_capable", capable, &bytes) != ESP_OK || bytes != sizeof(capable))
            memset(capable, 0, sizeof(capable));
        nvs_close(h);
    } else if (e != ESP_ERR_NVS_NOT_FOUND) storage_ok = false;
    ESP_ERROR_CHECK(esp_wifi_set_channel((uint8_t)current, WIFI_SECOND_CHAN_NONE));
    kms_radio_search_init(&search, current, kms_now());
    if (!storage_ok) ESP_LOGW(TAG, "Saved channel unavailable; using default; changes blocked until repaired");
}
void kms_radio_channel_forget(unsigned slot)
{
    if (!master || !slot || slot > KMS_SLOTS) return;
    memset(&peers[slot - 1], 0, sizeof(peers[slot - 1]));
}
void kms_radio_channel_peer(unsigned slot, const uint8_t *mac, const uint8_t *key)
{
    if (!slot || slot > KMS_SLOTS) return;
    channel_peer_t *p = &peers[master ? slot - 1 : 0];
    if (!p->valid || memcmp(p->mac, mac, 6)) memset(p, 0, sizeof(*p));
    p->valid = true; memcpy(p->mac, mac, 6); memcpy(p->key, key, KMS_LINK_KEY_BYTES);
    if (!master) p->reply.slot = (uint8_t)slot;
}
bool kms_radio_channel_receive(const uint8_t *mac, const uint8_t *wire, size_t bytes,
                               uint32_t received_at, kms_radio_message_t *probe_reply)
{
    kms_radio_message_t m; uint8_t proof[KMS_RADIO_TAG], difference = 0;
    if (!kms_radio_decode(&m, wire, bytes)) return false;
    channel_peer_t *p = &peers[master ? m.slot - 1 : 0];
    if (!p->valid || memcmp(p->mac, mac, 6) || !tag(p->key, wire, proof)) return false;
    for (unsigned i = 0; i < sizeof(proof); ++i) difference |= proof[i] ^ wire[KMS_RADIO_HEADER + i];
    if (difference) return false;
    if (master && m.type == KMS_RADIO_QUERY && m.channel == current) {
        if (memcmp(capable[m.slot - 1], mac, 6)) {
            uint8_t updated[KMS_SLOTS][6]; memcpy(updated, capable, sizeof(updated));
            memcpy(updated[m.slot - 1], mac, 6);
            if (store("radio_capable", updated, sizeof(updated))) memcpy(capable, updated, sizeof(capable));
        }
        p->reply = m; p->reply.type = KMS_RADIO_REPLY; p->pending = true;
    } else if (!master && m.type == KMS_RADIO_REPLY && m.slot == p->reply.slot &&
               m.boot == boot_id && nonce && m.nonce == nonce && kms_now() - query_at < KMS_RADIO_DWELL_MS &&
               kms_radio_channel_valid(m.channel, first_channel, channel_count)) {
        nonce = 0;
        kms_radio_search_contact(&search, kms_now());
        if (m.channel != current) {
            if (!changing) release_input();
            requested = m.channel; switch_at = kms_now() + m.delay_ms + 50u; changing = true;
            publish_interlock();
        } else if (!changing && !save_channel(current)) ESP_LOGW(TAG, "Discovered channel not saved; will search after restart");
    } else if (m.channel == current && !changing) {
        if (master && m.type == KMS_RADIO_PROBE_REPLY && probe_reply) {
            *probe_reply = m;
            return true;
        }
        if (!master && m.type == KMS_RADIO_PROBE && m.boot == boot_id && m.slot == p->reply.slot) {
            pending_probe = m;
            pending_probe.type = KMS_RADIO_PROBE_REPLY;
            probe_received_at = received_at;
        }
    }
    return false;
}
bool kms_radio_channel_probe(unsigned slot, uint32_t boot, uint32_t challenge)
{
    if (!master || changing || !slot || slot > KMS_SLOTS || !peers[slot - 1].valid) return false;
    kms_radio_message_t m = {.type = KMS_RADIO_PROBE, .channel = (uint8_t)current,
                             .slot = (uint8_t)slot, .boot = boot, .nonce = challenge};
    return transmit(&peers[slot - 1], &m);
}
void kms_radio_channel_processing_fault(uint32_t now)
{
    processing_fault_seen = true;
    processing_fault_at = now;
}
void kms_radio_channel_probe_reply_tick(bool pairing)
{
    if (master || !pending_probe.nonce) return;
    uint32_t now = kms_now(), age = now - probe_received_at;
    if (pairing || changing || kms_ota_busy() || kms_remote_restart_pending() ||
        age >= KMS_RADIO_PROBE_TIMEOUT_MS) {
        pending_probe.nonce = 0;
        return;
    }
    pending_probe.delay_ms = (uint16_t)age;
    pending_probe.flags = processing_fault_seen && now - processing_fault_at < RECEIVER_PROCESSING_QUIET_MS ?
        KMS_RADIO_PROBE_BUSY : 0;
    if (transmit(&peers[0], &pending_probe)) pending_probe.nonce = 0;
}
void kms_radio_channel_paired(void)
{
    kms_radio_search_contact(&search, kms_now());
    if (!save_channel(current)) ESP_LOGW(TAG, "Paired channel could not be saved");
}
void kms_radio_channel_contact(void)
{
    if (!master && !changing) kms_radio_search_contact(&search, kms_now());
}
bool kms_radio_channel_changing(void) { return (read_interlock() & SWITCH_HELD) != 0; }
bool kms_radio_channel_switch_pending(void) { return changing; }
bool kms_radio_channel_trial_active(void) { return trial; }
bool kms_radio_channel_recovering(void) { return read_interlock() != 0; }
unsigned kms_radio_channel_get(void) { return current; }
static bool all_capable(void)
{
    for (unsigned i = 0; i < KMS_SLOTS; ++i)
        if (peers[i].valid && memcmp(capable[i], peers[i].mac, 6)) return false;
    return true;
}
static void announce(unsigned channel)
{
    requested = channel;
    switch_at = kms_now() + KMS_RADIO_SWITCH_MS;
    changing = true;
    publish_interlock();
}
bool kms_radio_channel_trial_available(void)
{
    return master && !kms_radio_channel_recovering() && storage_ok && all_capable() &&
        !kms_link_receiver_restart_in_progress() && !kms_link_pairing() &&
        !kms_remote_transfer_busy() && !kms_test_mode_active();
}
bool kms_radio_channel_trial_switch(unsigned channel)
{
    if (!master || changing || channel == current || !kms_radio_channel_valid(channel, first_channel, channel_count) ||
        (!trial && !kms_radio_channel_trial_available())) return false;
    trial = true;
    announce(channel);
    return true;
}
void kms_radio_channel_trial_cancel_switch(void)
{
    if (trial) {
        changing = false;
        for (unsigned i = 0; i < KMS_SLOTS; ++i) peers[i].pending = false;
    }
}
bool kms_radio_channel_trial_commit(void) { return trial && !changing && save_channel(current); }
void kms_radio_channel_trial_end(void)
{
    kms_radio_channel_trial_cancel_switch();
    trial = false;
    manual_retuned_at = kms_now();
    manual_grace = true;
    publish_interlock();
}
const char *kms_radio_channel_set(unsigned channel)
{
    if (!master || !kms_radio_channel_valid(channel, first_channel, channel_count)) return "INVALID";
    if ((changing && !trial) || kms_link_pairing() || kms_remote_transfer_busy() || kms_test_mode_active()) return "BUSY";
    if (!storage_ok) return "STORAGE";
    if (channel == current && !trial) return save_channel(channel) ? NULL : "STORAGE";
    if (kms_link_receiver_restart_in_progress()) return "BUSY";
    if (channel != current && !all_capable()) return "UPGRADE_SLAVES";
    if (!save_channel(channel)) return "STORAGE";
    if (trial) kms_radio_channel_trial_end();
    if (channel == current) return NULL;
    kms_link_pause();
    announce(channel);
    return NULL;
}
void kms_radio_channel_snapshot(kms_radio_channel_status_t *out)
{
    unsigned paired = 0, supported = 0;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) if (peers[i].valid) {
        ++paired; if (!memcmp(capable[i], peers[i].mac, 6)) ++supported;
    }
    *out = (kms_radio_channel_status_t){current, changing ? requested : current,
        first_channel, channel_count, paired, supported};
}
void kms_radio_channel_status(unsigned long id, const char *result)
{
    kms_radio_channel_status_t s;
    kms_radio_channel_snapshot(&s);
    printf("KMSCHANNEL %lu %s %u %u %u %u %u %u\n", id, result, s.current, s.pending,
        s.first, s.count, s.paired, s.supported);
}
void kms_radio_channel_tick(bool pairing, bool candidate, bool busy)
{
    uint32_t now = kms_now();
    if (manual_grace && now - manual_retuned_at >= CHANNEL_RECONNECT_MS) {
        manual_grace = false;
        publish_interlock();
    }
    if (busy) return; // Never retune with a transmit callback outstanding.
    if (!master && was_pairing && !pairing && !changing) {
        (void)retune(saved ? saved : KMS_RADIO_CHANNEL);
        kms_radio_search_init(&search, current, now);
    }
    was_pairing = pairing;
    if (changing && (int32_t)(now - switch_at) >= 0) {
        changing = false;
        if (retune(requested)) {
            kms_radio_search_init(&search, current, now);
            ESP_LOGI(TAG, "Channel %u active%s", current, master && !trial ? "; routing paused, resume when ready" : "");
        } else if (master && !trial) (void)save_channel(current);
        if (master && !trial) {
            manual_retuned_at = now;
            manual_grace = true;
        }
        publish_interlock();
    }
    if (master) {
        for (unsigned n = 0; n < KMS_SLOTS; ++n) {
            unsigned i = next_reply++ % KMS_SLOTS; channel_peer_t *p = &peers[i];
            if (!p->pending) continue;
            p->reply.channel = (uint8_t)(changing ? requested : current);
            p->reply.delay_ms = changing ? (uint16_t)(switch_at - now) : 0;
            if (transmit(p, &p->reply)) p->pending = false;
            return;
        }
        return;
    }
    bool freeze = candidate || changing || kms_ota_busy();
    unsigned channel = kms_radio_search_tick(&search, now, first_channel, channel_count, pairing, peers[0].valid, freeze);
    if (channel != current) {
        release_input();
        if (!retune(channel)) search.channel = (uint8_t)current;
    }
    unsigned interval = search.searching ? KMS_RADIO_QUERY_MS : KMS_RADIO_HEARTBEAT_MS;
    if (!pairing && peers[0].valid && !changing && now - query_at >= interval) {
        uint32_t challenge; do { challenge = esp_random(); } while (!challenge);
        kms_radio_message_t m = {.type = KMS_RADIO_QUERY, .channel = (uint8_t)current,
            .slot = peers[0].reply.slot, .boot = boot_id, .nonce = challenge};
        if (transmit(&peers[0], &m)) { nonce = challenge; query_at = now; }
    }
}
