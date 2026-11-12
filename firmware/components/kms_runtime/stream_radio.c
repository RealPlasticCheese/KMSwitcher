#include "kms_stream_radio.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "kms_memory.h"
#include "esp_wifi.h"
#include <string.h>

typedef struct {
    uint32_t boot, hello_at;
    uint8_t requested, effective, failures, source_profile;
    bool supported, hello_sent, reply_pending;
} stream_peer_t;
static stream_peer_t peers[7];
static uint32_t boot, sequence, last_send;
static kms_stream_space_fn space;
static kms_stream_receive_fn deliver;
// The Wi-Fi driver copies sends synchronously. This single internal staging
// buffer is reused; PSRAM-backed slots never cross the driver/flash boundary.
_Static_assert(KMS_STREAM_MAX_WIRE <= 1470, "Bulk batches must fit ESP-NOW v2");
static uint8_t wire[KMS_STREAM_MAX_WIRE];
static kms_stream_batch_t *batch;
static unsigned indices[KMS_STREAM_WINDOW];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned active_slot;
static bool active_bulk;

void kms_stream_radio_init(uint32_t own_boot, kms_stream_space_fn available, kms_stream_receive_fn receive)
{ boot = own_boot; space = available; deliver = receive; batch = kms_memory_calloc(1, sizeof(*batch)); }
void kms_stream_radio_receive(const uint8_t *data, size_t bytes, const uint8_t *mac, int8_t rssi)
{
    // Wi-Fi callbacks are serialized. Decode on their internal stack, validate
    // the entire envelope, then admit all cells or none into the existing queue.
    kms_stream_batch_t received;
    if (!space || !deliver || !kms_stream_decode(&received, data, bytes) || received.target != boot ||
        space() < (unsigned)(received.count ? received.count : 1)) return;
    if (received.flags) {
        kms_management_frame_t hello = {.op = KMS_M_STREAM_STATUS, .slot = received.slot,
            .id = received.sequence, .sender = received.sender, .target = received.target,
            .flags = KMS_M_REPLY, .length = 4};
        memcpy(hello.data, "KB", 2); hello.data[2] = received.flags; hello.data[3] = received.profile;
        deliver(&hello, mac, rssi); return;
    }
    for (unsigned i = 0; i < received.count; ++i) deliver(&received.frames[i], mac, rssi);
}
bool kms_stream_radio_control(const kms_management_frame_t *f)
{
    if (f->op != KMS_M_STREAM_STATUS || f->length != 4 || memcmp(f->data, "KB", 2)) return false;
    if (!f->slot || f->slot > 7 || f->target != boot) return true;
    portENTER_CRITICAL(&lock);
    stream_peer_t *p = &peers[f->slot - 1];
    if (p->boot != f->sender) *p = (stream_peer_t){.boot = f->sender};
    p->supported = true;
    // Capability responses are control traffic, even on a receive-only dongle.
    if (f->data[2] == KMS_STREAM_HELLO) {
        p->reply_pending = true; p->source_profile = f->data[3];
    }
    portEXIT_CRITICAL(&lock); return true;
}
void kms_stream_radio_complete(bool success, uint32_t elapsed)
{
    portENTER_CRITICAL(&lock);
    if (active_bulk && active_slot) {
        stream_peer_t *p = &peers[active_slot - 1];
        if (!success || elapsed >= 100) {
            if (++p->failures >= 3) { if (p->effective) --p->effective; p->failures = 0; }
        } else p->failures = 0;
    }
    active_bulk = false;
    portEXIT_CRITICAL(&lock);
}
void kms_stream_radio_rate(const uint8_t *mac, const uint8_t *data, size_t bytes)
{
    bool bulk = bytes >= KMS_STREAM_HEADER && !memcmp(data, "KMB2", 4) && !data[5];
    unsigned profile = bulk ? data[7] : KMS_STREAM_PROFILE_CONSERVATIVE;
    esp_now_rate_config_t rate = {.phymode = profile ? WIFI_PHY_MODE_11G : WIFI_PHY_MODE_11B,
        .rate = profile == KMS_STREAM_PROFILE_FAST ? WIFI_PHY_RATE_12M :
                profile == KMS_STREAM_PROFILE_BALANCED ? WIFI_PHY_RATE_6M : WIFI_PHY_RATE_1M_L};
    if (esp_now_set_peer_rate_config(mac, &rate) != ESP_OK) {
        rate.phymode = WIFI_PHY_MODE_11B; rate.rate = WIFI_PHY_RATE_1M_L;
        (void)esp_now_set_peer_rate_config(mac, &rate);
        if (bulk && data[21] && data[21] <= 7) {
            portENTER_CRITICAL(&lock); peers[data[21] - 1].effective = 0; portEXIT_CRITICAL(&lock);
        }
    }
    portENTER_CRITICAL(&lock);
    active_bulk = bulk; active_slot = bulk ? data[21] : 0;
    portEXIT_CRITICAL(&lock);
}
void kms_stream_radio_tick(bool input_idle, uint32_t now, kms_stream_send_fn send, kms_stream_peer_fn resolve)
{
    // Even a small advertisement or capability reply can occupy the shared
    // transmitter for a long retry. Preserve the input acknowledgement window
    // for every stream frame, not just cells classified as bulk data.
    if (!batch || !input_idle) return;
    for (unsigned i = 0; i < 7; ++i) {
        portENTER_CRITICAL(&lock);
        bool pending = peers[i].reply_pending; uint32_t target = peers[i].boot;
        portEXIT_CRITICAL(&lock);
        if (!pending) continue;
        uint8_t mac[6]; uint32_t current;
        if (!resolve(i + 1, mac, &current) || (current && current != target)) continue;
        memset(batch, 0, sizeof(*batch)); batch->sender = boot; batch->target = target;
        batch->slot = (uint8_t)(i + 1); batch->flags = KMS_STREAM_HELLO_REPLY;
        batch->sequence = ++sequence ? sequence : ++sequence; batch->credit = KMS_STREAM_WINDOW;
        size_t bytes = kms_stream_encode(batch, wire, sizeof(wire));
        if (bytes && send(mac, wire, bytes)) {
            portENTER_CRITICAL(&lock); peers[i].reply_pending = false; portEXIT_CRITICAL(&lock);
        }
        return;
    }
    memset(batch, 0, sizeof(*batch));
    unsigned n = kms_stream_select(false, indices, batch, 1);
    if (!n) n = kms_stream_select(true, indices, batch, KMS_STREAM_WINDOW);
    if (!n) return;
    uint8_t mac[6]; uint32_t current_boot;
    if (!resolve(batch->slot, mac, &current_boot) || (current_boot && current_boot != batch->target)) {
        kms_stream_commit(indices, n); return; // Stale boot/removed peer can never receive queued data.
    }
    bool bulk = kms_stream_bulk(&batch->frames[0]);
    unsigned profile = KMS_STREAM_PROFILE_CONSERVATIVE;
    bool supported, hello;
    portENTER_CRITICAL(&lock);
    stream_peer_t *p = &peers[batch->slot - 1];
    if (p->boot != batch->target) *p = (stream_peer_t){.boot = batch->target};
    if (bulk && p->requested != batch->profile) {
        p->requested = p->effective = batch->profile; p->failures = 0; p->hello_sent = false;
    }
    supported = p->supported; profile = supported ? p->effective : 0;
    hello = bulk && (!p->hello_sent || (!supported && now - p->hello_at >= 5000));
    portEXIT_CRITICAL(&lock);
    kms_stream_profile_t settings; kms_stream_profile(profile, &settings);
    if (now - last_send < (bulk ? settings.pace_ms : 10u)) return;
    if (hello) {
        batch->sender = boot; batch->count = 0; batch->flags = KMS_STREAM_HELLO;
        batch->sequence = ++sequence ? sequence : ++sequence; batch->credit = KMS_STREAM_WINDOW;
        size_t bytes = kms_stream_encode(batch, wire, sizeof(wire));
        if (bytes && send(mac, wire, bytes)) {
            last_send = now;
            portENTER_CRITICAL(&lock); p->hello_sent = true; p->hello_at = now; portEXIT_CRITICAL(&lock);
        }
        return;
    }
    size_t bytes;
    if (bulk && supported) {
        if (n > settings.window) n = settings.window;
        batch->count = (uint8_t)n; batch->profile = (uint8_t)profile; batch->credit = KMS_STREAM_WINDOW;
        batch->sequence = ++sequence ? sequence : ++sequence;
        bytes = kms_stream_encode(batch, wire, sizeof(wire));
    } else { n = 1; bytes = kms_management_encode(&batch->frames[0], wire, sizeof(wire)); }
    if (bytes && send(mac, wire, bytes)) { kms_stream_commit(indices, n); last_send = now; }
}
unsigned kms_stream_radio_source_profile(unsigned slot)
{
    portENTER_CRITICAL(&lock); unsigned profile = slot && slot <= 7 ? peers[slot - 1].source_profile : 0;
    portEXIT_CRITICAL(&lock); return profile;
}
size_t kms_stream_radio_status(uint8_t *out, size_t capacity)
{
    size_t n = kms_stream_usb_get(out, capacity);
    if (n < 32) return n;
    portENTER_CRITICAL(&lock);
    for (unsigned i = 0; i < 7; ++i) {
        out[16 + i] = peers[i].supported ? peers[i].effective : 0;
        if (peers[i].boot) out[24] |= (uint8_t)(1u << i);
        if (peers[i].supported) out[23] |= (uint8_t)(1u << i);
    }
    portEXIT_CRITICAL(&lock); return n;
}

uint32_t kms_stream_local_status(unsigned slot, unsigned incoming)
{
    unsigned capacity = kms_stream_capacity(), profile = 255;
    if (incoming > KMS_STREAM_WINDOW) incoming = KMS_STREAM_WINDOW;
    portENTER_CRITICAL(&lock);
    if (slot && slot <= 7 && peers[slot - 1].supported) profile = peers[slot - 1].effective;
    portEXIT_CRITICAL(&lock);
    return KMS_STREAM_STATUS_TAG | (incoming << 16) | (capacity << 8) | profile;
}
