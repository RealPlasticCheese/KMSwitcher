#include "kms_runtime.h"
#include "kms_jiggler.h"
#include "kms_jiggler_store.h"
#include "kms_radio_channel.h"
#include "kms_channel_recovery.h"
#include "pair_crypto.h"
#include "kms_delivery.h"
#include "kms_ack.h"
#include "kms_diagnostic_store.h"
#include "kms_remote.h"
#include "kms_slave_diagnostics.h"
#include "kms_ota.h"
#include "kms_test.h"
#include "kms_test_mode.h"
#include "kms_memory.h"
#include "kms_desktop_runtime.h"
#include "kms_stream_radio.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#if KMS_SLOTS > CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM
#error SDK encrypted peer capacity must cover all configured computer slots
#endif
_Static_assert(KMS_MAX_WIRE_SIZE <= ESP_NOW_MAX_DATA_LEN, "Input batches must fit legacy ESP-NOW frames");

enum { PEER_STORE_VERSION = 1 };
typedef struct { uint8_t valid, slot, mac[6], key[KMS_LINK_KEY_BYTES]; } record_t;
typedef struct { uint32_t version; record_t records[KMS_SLOTS]; } store_t;
typedef struct { record_t record; uint32_t boot, seen; uint8_t flags, batch_limit; int8_t rssi; bool suspended; } peer_t;
typedef struct { uint8_t mac[6]; int8_t rssi; kms_packet_t packet; } received_t;
// Only receivers accept batched input. Both roles retain the same queue depth.
#if CONFIG_IDF_TARGET_ESP32S3
enum { INPUT_RX_BYTES = KMS_WIRE_SIZE };
#else
enum { INPUT_RX_BYTES = KMS_MAX_WIRE_SIZE };
#endif
typedef struct { uint8_t mac[6]; int8_t rssi; uint8_t length; uint32_t received_at; uint8_t wire[INPUT_RX_BYTES]; } queued_input_t;
typedef struct { uint8_t mac[6]; int8_t rssi; kms_management_frame_t frame; } management_received_t;
typedef struct { uint32_t received_at; uint8_t mac[6], wire[KMS_RADIO_BYTES]; } channel_received_t;
static const char *TAG = "link";
static const uint8_t broadcast[6] = {255,255,255,255,255,255};
// Firmware roles are fixed by the board target. Keep the S3 delivery/recovery
// state out of the S2 image instead of retaining an unused controller buffer.
#if CONFIG_IDF_TARGET_ESP32S3
static const bool master = true;
#else
static const bool master = false;
#endif
static bool ready, fault, initial_selected, recovery_warning_started, error_active;
static bool usb_suspended, receiver_host_available;
static uint8_t own_mac[6];
static unsigned boot_slot;
static uint32_t boot_id, last_status, error_until;
static kms_delivery_t sender;
static peer_t peers[KMS_SLOTS];
static kms_receiver_guard_t guard;
static bool desktop_idle = true;
static kms_jiggler_t jiggler;
static kms_jiggler_sync_t jiggler_peers[KMS_SLOTS];
static uint16_t jiggler_seconds = KMS_JIGGLER_DEFAULT_SECONDS;
static uint8_t jiggler_distance = 1;
static unsigned jiggler_next;
static kms_ack_t receipt;
static kms_ack_t text_receipt;
static kms_text_tx_t text_tx;
static kms_text_rx_t text_rx;
static uint32_t text_sequence;
static kms_led_state_t keyboard_leds;
static kms_led_feedback_t led_feedback;
static uint32_t last_led_sequence, last_led_status;
static kms_deliver_fn deliver;
static void (*clear_output)(void);
static QueueHandle_t incoming;
static QueueHandle_t management_incoming;
static QueueHandle_t channel_incoming;
static uint32_t last_management, last_selection;
static unsigned paused_slot;
static uint32_t route_epoch, recovery_resume_epoch;
#if CONFIG_IDF_TARGET_ESP32S3
static struct { unsigned slot; uint32_t epoch, after; bool waiting, manual; } channel_route;
static uint32_t processing_fault_at;
static bool processing_fault_seen;
#endif
static unsigned recovery_owner, recovery_resume_slot;
static uint32_t recovery_global_at;
static bool recovery_global_wait;
static kms_receiver_recovery_t receiver_recovery[KMS_SLOTS];
static uint8_t recovery_macs[KMS_SLOTS][6];
static bool recovery_initialized[KMS_SLOTS];
static bool recovery_online[KMS_SLOTS];
static uint32_t recovery_seen_errors[KMS_SLOTS], recovery_errors[KMS_SLOTS], recovery_reasons[KMS_SLOTS];
static uint32_t radio_rejected_at;
static bool radio_rejection_seen;
static bool rx_overflow, tx_busy;
typedef struct {
    uint32_t started, submitted, completed, failed, rejected, unexpected, max_ms, last_error, bytes;
    uint32_t recoveries, window_since, attempts;
    uint32_t sequence;
    uint8_t mac[6];
    uint8_t slot, kind, op;
    bool blocked, bulk, probe;
} radio_tx_state_t;
static radio_tx_state_t radio_tx;
enum { RADIO_INPUT, RADIO_DESKTOP, RADIO_CLIPBOARD, RADIO_OTHER, RADIO_KINDS, RADIO_SLOW_RECORDS = 16 };
typedef struct { uint32_t completed, failed, max_ms; } radio_class_t;
static radio_class_t radio_peers[KMS_SLOTS][RADIO_KINDS];
typedef struct {
    uint32_t at, ms, sequence;
    uint16_t bytes;
    uint8_t slot, kind, op, failed;
} radio_slow_t;
static radio_slow_t radio_slow[RADIO_SLOW_RECORDS];
static unsigned radio_slow_next;
static uint32_t clipboard_deferred, last_radio_trace;
static bool radio_trace_seen;
static uint32_t radio_rx_last_ms, radio_rx_max_ms;
static uint32_t radio_tx_last_ms, radio_tx_slow, radio_rx_frames, radio_rx_at, radio_rx_gap;
static uint32_t radio_tick_at, radio_tick_max;
static portMUX_TYPE tx_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE rx_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool open, candidate, encrypted, complete;
    unsigned slot;
    uint32_t until, last, key_at, remote_boot;
    record_t record;
    kms_exchange_t exchange;
    uint8_t nonce[KMS_NONCE_BYTES];
} pairing;

static bool same(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, ESP_NOW_ETH_ALEN) == 0; }
static esp_err_t configure_peer(const uint8_t *mac, const uint8_t *key)
{
    esp_now_peer_info_t p = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = key != NULL};
    memcpy(p.peer_addr, mac, sizeof(p.peer_addr));
    if (key) memcpy(p.lmk, key, sizeof(p.lmk));
    return esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&p) : esp_now_add_peer(&p);
}
static void radio_peer(const uint8_t *mac, const uint8_t *key)
{ ESP_ERROR_CHECK(configure_peer(mac, key)); }
static unsigned radio_kind(const uint8_t *wire, size_t bytes, uint8_t *op, uint32_t *sequence)
{
    *op = 0; *sequence = 0;
    if (!wire || bytes < 24) return RADIO_OTHER;
    if (!memcmp(wire, "KMSW", 4)) {
        *op = wire[5]; *sequence = kms_get_u32(wire + 20);
        return wire[5] == KMS_INPUT || wire[5] == KMS_ACTIVATE || wire[5] == KMS_RELEASE ||
               wire[5] == KMS_TEXT ? RADIO_INPUT : RADIO_OTHER;
    }
    if (!memcmp(wire, "KMSB", 4)) {
        *op = KMS_INPUT_BATCH; *sequence = kms_get_u32(wire + 20); return RADIO_INPUT;
    }
    if (bytes >= KMS_M_HEADER && !memcmp(wire, "KMSM", 4)) {
        *op = wire[5]; *sequence = kms_get_u32(wire + 12);
        if (wire[5] == KMS_M_DESKTOP) {
            if (bytes >= KMS_M_HEADER + 3) *op = wire[KMS_M_HEADER + 2];
            return RADIO_DESKTOP;
        }
        if (wire[5] == KMS_M_CLIPBOARD) {
            if (bytes >= KMS_M_HEADER + 5) *op = wire[KMS_M_HEADER + 4];
            return RADIO_CLIPBOARD;
        }
    }
    return RADIO_OTHER;
}
static void sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    uint32_t now = kms_now();
    uint32_t fault_ms = 0, failures = 0; unsigned fault_op = 0, fault_reason = 0;
    // MAC completion frees the transmitter; application ACKs still confirm input.
    portENTER_CRITICAL(&tx_lock);
    if (tx_busy && !radio_tx.blocked) {
        uint32_t elapsed = now - radio_tx.started;
        radio_tx_last_ms = elapsed;
        if (elapsed >= 100u) ++radio_tx_slow;
        if (elapsed > radio_tx.max_ms) radio_tx.max_ms = elapsed;
        ++radio_tx.completed;
        if (status != ESP_NOW_SEND_SUCCESS) ++radio_tx.failed;
        if (!master && (status != ESP_NOW_SEND_SUCCESS || elapsed >= 100u)) {
            fault_ms = elapsed; failures = radio_tx.failed; fault_op = radio_tx.op;
            fault_reason = status != ESP_NOW_SEND_SUCCESS ? KMS_RF_MAC_FAILED : KMS_RF_TX_SLOW;
        }
        if (radio_tx.slot) {
            radio_class_t *p = &radio_peers[radio_tx.slot - 1][radio_tx.kind];
            ++p->completed; if (status != ESP_NOW_SEND_SUCCESS) ++p->failed;
            if (elapsed > p->max_ms) p->max_ms = elapsed;
            if (master && !radio_tx.bulk && !radio_tx.probe && (status != ESP_NOW_SEND_SUCCESS || elapsed >= 100u)) {
                unsigned i = radio_tx.slot - 1;
                ++recovery_errors[i];
                recovery_reasons[i] |= status != ESP_NOW_SEND_SUCCESS ? KMS_RECOVERY_MAC : KMS_RECOVERY_SLOW;
            }
        }
#if CONFIG_IDF_TARGET_ESP32S3
        if (radio_tx.probe) kms_channel_recovery_mac(radio_tx.sequence, radio_tx.started, elapsed, status != ESP_NOW_SEND_SUCCESS);
#endif
        if (elapsed >= KMS_RADIO_RETRY_MS || status != ESP_NOW_SEND_SUCCESS) {
            radio_slow[radio_slow_next++ % RADIO_SLOW_RECORDS] = (radio_slow_t){
                .at = now, .ms = elapsed, .sequence = radio_tx.sequence, .bytes = (uint16_t)radio_tx.bytes,
                .slot = radio_tx.slot, .kind = radio_tx.kind, .op = radio_tx.op,
                .failed = status != ESP_NOW_SEND_SUCCESS};
        }
        kms_stream_radio_complete(status == ESP_NOW_SEND_SUCCESS, elapsed);
        tx_busy = false;
    } else ++radio_tx.unexpected;
    portEXIT_CRITICAL(&tx_lock);
    if (fault_reason) kms_slave_diagnostics_fault(fault_reason, fault_op, fault_ms, failures);
    kms_app_wake();
}
static void radio_receive_timing(uint32_t received_at)
{
    uint32_t now = kms_now();
    portENTER_CRITICAL(&rx_lock);
    if (radio_rx_frames && now - radio_rx_at > radio_rx_gap) radio_rx_gap = now - radio_rx_at;
    ++radio_rx_frames; radio_rx_at = now;
    uint32_t age = now - received_at;
    if (age > radio_rx_max_ms) radio_rx_max_ms = age;
    radio_rx_last_ms = age;
    portEXIT_CRITICAL(&rx_lock);
    if (!master && now > 5000u && age >= 50u)
        kms_slave_diagnostics_fault(KMS_RF_DISPATCH, 0, age, radio_tx.failed);
}
size_t kms_link_radio_status(uint8_t *out, size_t capacity)
{
    if (capacity < KMS_RADIO_STATUS_WORDS * 4u) return 0;
    uint32_t now = kms_now(), words[KMS_RADIO_STATUS_WORDS] = {1, boot_id, now};
    portENTER_CRITICAL(&tx_lock);
    words[3] = radio_tx.submitted; words[4] = radio_tx.completed; words[5] = radio_tx.failed;
    words[6] = radio_tx_slow; words[7] = radio_tx.rejected; words[8] = radio_tx.max_ms;
    words[9] = radio_tx_last_ms; words[10] = tx_busy ? now - radio_tx.started : 0;
    portEXIT_CRITICAL(&tx_lock);
    portENTER_CRITICAL(&rx_lock);
    words[11] = radio_rx_frames; words[12] = radio_rx_frames ? now - radio_rx_at : 0;
    words[13] = radio_rx_gap; words[14] = radio_rx_max_ms; words[15] = radio_rx_last_ms;
    words[16] = radio_tick_max;
    portEXIT_CRITICAL(&rx_lock);
    words[17] = incoming ? uxQueueMessagesWaiting(incoming) : 0;
    words[18] = ready; words[19] = esp_get_minimum_free_heap_size();
    for (unsigned i = 0; i < KMS_RADIO_STATUS_WORDS; ++i) kms_put_u32(out + i * 4, words[i]);
    return sizeof(words);
}
static bool send_frame(const uint8_t *mac, const uint8_t *wire, size_t bytes)
{
    // Keep at most one frame in the driver. Offline-peer retries must not build
    // a FIFO that delays activation of the next selected computer.
    portENTER_CRITICAL(&tx_lock);
    if (tx_busy || radio_tx.blocked) { portEXIT_CRITICAL(&tx_lock); return false; }
    tx_busy = true; radio_tx.started = kms_now();
    radio_tx.bytes = (uint32_t)bytes;
    radio_tx.bulk = bytes >= KMS_STREAM_HEADER && !memcmp(wire, "KMB2", 4) && !wire[5];
    radio_tx.kind = (uint8_t)radio_kind(wire, bytes, &radio_tx.op, &radio_tx.sequence);
    radio_tx.probe = master && bytes == KMS_RADIO_BYTES && wire[5] == KMS_RADIO_PROBE && !memcmp(wire, "KMSC", 4);
    if (radio_tx.probe) radio_tx.sequence = kms_get_u32(wire + 12);
    radio_tx.slot = 0;
    for (unsigned i = 0; i < KMS_SLOTS; ++i)
        if (peers[i].record.valid && same(mac, peers[i].record.mac)) { radio_tx.slot = (uint8_t)(i + 1); break; }
    memcpy(radio_tx.mac, mac, sizeof(radio_tx.mac));
    portEXIT_CRITICAL(&tx_lock);
#ifdef CONFIG_KMS_TEST_SUPPORT
    int64_t call_started = esp_timer_get_time();
#endif
    kms_stream_radio_rate(mac, wire, bytes);
    esp_err_t e = esp_now_send(mac, wire, bytes);
#ifdef CONFIG_KMS_TEST_SUPPORT
    kms_test_record(KMS_T_RADIO_SEND_CALL, (uint32_t)bytes,
                    (uint32_t)(esp_timer_get_time() - call_started), (uint32_t)e, NULL, 0);
#endif
    if (!master) {
        if (e == ESP_OK) kms_slave_diagnostics_add(KMS_S_TX, 1);
        if (e != ESP_OK) kms_slave_diagnostics_event(KMS_S_RADIO_ERROR, (uint32_t)e);
    }
    portENTER_CRITICAL(&tx_lock);
    if (e == ESP_OK) ++radio_tx.submitted;
    else {
        ++radio_tx.rejected; radio_tx.last_error = (uint32_t)e;
        kms_stream_radio_complete(false, 0); tx_busy = false;
        radio_rejected_at = kms_now(); radio_rejection_seen = true;
    }
    portEXIT_CRITICAL(&tx_lock);
    if (e == ESP_OK) return true;
    ESP_LOGD(TAG, "Radio send: %s", esp_err_to_name(e)); return false;
}
static void receive(const esp_now_recv_info_t *info, const uint8_t *data, int len);
static esp_err_t restart_radio(void)
{
    // ESP-NOW deinit deletes its peer table. Stop the old driver before freeing
    // the transmit gate, then restore the same encrypted peers and channel.
    esp_err_t e = esp_now_deinit();
    if (e == ESP_OK) e = esp_wifi_stop();
    if (e == ESP_OK) e = esp_wifi_start();
    if (e == ESP_OK) e = esp_wifi_set_ps(WIFI_PS_NONE);
    if (e == ESP_OK) e = esp_wifi_set_channel(kms_radio_channel_get(), WIFI_SECOND_CHAN_NONE);
    if (e == ESP_OK) e = esp_now_init();
    if (e == ESP_OK) e = esp_now_register_recv_cb(receive);
    if (e == ESP_OK) e = esp_now_register_send_cb(sent);
    if (e == ESP_OK) e = configure_peer(broadcast, NULL);
    for (unsigned i = 0; e == ESP_OK && i < KMS_SLOTS; ++i)
        if (peers[i].record.valid) e = configure_peer(peers[i].record.mac, peers[i].record.key);
    return e;
}
static void radio_health_tick(void)
{
    // An automatic trial can give up its pending retune to the driver watchdog.
    if (!master || pairing.open || kms_remote_transfer_busy() || !kms_ota_confirmed() ||
        (kms_radio_channel_changing() && !kms_radio_channel_trial_active()) || kms_test_mode_active()) return;
    uint32_t now = kms_now();
    portENTER_CRITICAL(&tx_lock);
    if (!tx_busy || radio_tx.blocked || !kms_time_elapsed(now, radio_tx.started, KMS_RADIO_TX_TIMEOUT_MS)) {
        portEXIT_CRITICAL(&tx_lock); return;
    }
    radio_tx.blocked = true;
    if (kms_time_elapsed(now, radio_tx.window_since, KMS_RADIO_RECOVERY_WINDOW_MS)) {
        radio_tx.attempts = 0; radio_tx.window_since = now;
    }
    bool allowed = radio_tx.attempts < KMS_RADIO_RECOVERY_ATTEMPTS;
    if (allowed) ++radio_tx.attempts;
    radio_tx_state_t snapshot = radio_tx;
    portEXIT_CRITICAL(&tx_lock);
#if CONFIG_IDF_TARGET_ESP32S3
    kms_channel_recovery_driver_stall();
#endif
    ESP_LOGE(TAG, "Radio completion missing for " MACSTR " age=%" PRIu32 "ms; attempt=%" PRIu32 "/%u",
        MAC2STR(snapshot.mac), now - snapshot.started, snapshot.attempts, KMS_RADIO_RECOVERY_ATTEMPTS);
    if (!allowed) {
        ESP_LOGE(TAG, "Radio recovery budget exhausted; restart the S3 after checking power and interference");
        return;
    }
    // Keep the route, generation and in-flight input intact. Retransmission of
    // its original sequence lets the receiver deduplicate an already sent input.
    esp_err_t e = restart_radio();
    portENTER_CRITICAL(&tx_lock);
    radio_tx.last_error = (uint32_t)e;
    if (e == ESP_OK) { tx_busy = false; radio_tx.blocked = false; ++radio_tx.recoveries; }
    portEXIT_CRITICAL(&tx_lock);
    if (e == ESP_OK) ESP_LOGW(TAG, "Radio restarted; retrying the existing route and sequence");
    else ESP_LOGE(TAG, "Radio recovery failed: %s; restart the S3", esp_err_to_name(e));
}
static bool send_packet(const uint8_t *mac, const kms_packet_t *p)
{
    if (kms_test_drop(p->type)) return true;
    uint8_t wire[KMS_MAX_WIRE_SIZE];
    size_t bytes = kms_wire_encode(p, wire, sizeof(wire));
    if (!bytes || !send_frame(mac, wire, bytes)) return false;
    if (p->type <= KMS_ACK || p->type == KMS_INPUT_BATCH)
        kms_test_record(KMS_T_TX, p->sequence, p->generation, p->slot, wire, 64);
    bool routed = master && (p == &sender.pending || p == &sender.release);
    kms_diagnostics_radio(true, routed && sender.retransmission, routed && sender.draining && kms_packet_is_input(p));
    return true;
}
static void receive(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !info->src_addr || !info->des_addr || !data || len < 4) return;
    bool unicast = same(info->des_addr, own_mac);
    if (!unicast && !same(info->des_addr, broadcast)) return;
    if (!memcmp(data, "KMSC", 4)) {
        if (len != KMS_RADIO_BYTES || !channel_incoming) return;
        channel_received_t message = {.received_at = kms_now()};
        memcpy(message.mac, info->src_addr, 6); memcpy(message.wire, data, sizeof(message.wire));
        (void)xQueueSend(channel_incoming, &message, 0); kms_app_wake(); return;
    }
    if (!memcmp(data, "KMB2", 4)) {
        if (unicast) kms_stream_radio_receive(data, (size_t)len, info->src_addr, info->rx_ctrl ? info->rx_ctrl->rssi : 0);
        kms_app_wake(); return;
    }
    if (!memcmp(data, "KMSM", 4)) {
        // ESP-NOW accepts unencrypted broadcasts even from an encrypted peer's
        // MAC. Only direct traffic may rely on the paired peer's CCMP protection.
        if (!unicast) return;
        management_received_t message;
        if (!management_incoming || !kms_management_decode(&message.frame, data, (size_t)len)) return;
        memcpy(message.mac, info->src_addr, sizeof(message.mac));
        message.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
        (void)xQueueSend(management_incoming, &message, 0);
        kms_app_wake();
        return;
    }
    if (len > INPUT_RX_BYTES) return;
#if CONFIG_IDF_TARGET_ESP32S3
    if (memcmp(data, "KMSW", 4)) return;
#endif
    received_t r;
    if (!kms_wire_decode(&r.packet, data, len)) return;
    // Enrollment starts with a broadcast; operational traffic must be unicast.
    if (!unicast && r.packet.type != KMS_PAIR_REQUEST) return;
    if (r.packet.type <= KMS_ACK || r.packet.type == KMS_INPUT_BATCH)
        kms_test_record(KMS_T_RX, r.packet.sequence, r.packet.generation, r.packet.slot, data, 64);
    queued_input_t message = {.length = (uint8_t)len, .rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0,
        .received_at = kms_now()};
    memcpy(message.mac, info->src_addr, sizeof(message.mac));
    memcpy(message.wire, data, (size_t)len);
    if (xQueueSend(incoming, &message, 0) != pdTRUE) {
        portENTER_CRITICAL(&rx_lock); rx_overflow = true; portEXIT_CRITICAL(&rx_lock);
    }
    kms_app_wake();
}
static void save(void)
{
    store_t s = {.version = PEER_STORE_VERSION};
    for (unsigned i = 0; i < KMS_SLOTS; ++i) s.records[i] = peers[i].record;
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, master ? "master" : "receiver", &s, sizeof(s)));
    ESP_ERROR_CHECK(nvs_commit(h)); nvs_close(h);
}
static void load(void)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h));
    store_t s = {0}; size_t n = sizeof(s);
    esp_err_t e = nvs_get_blob(h, master ? "master" : "receiver", &s, &n);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return;
    if (e != ESP_OK || n != sizeof(s) || s.version != PEER_STORE_VERSION) {
        ESP_LOGW(TAG, "Peer store format differs; re-pair receivers"); return;
    }
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        record_t *r = &s.records[i];
        if (!r->valid) continue;
        if (!r->slot || r->slot > KMS_SLOTS || (r->mac[0] & 1) || (master && r->slot != i + 1)) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < i; ++j) if (peers[j].record.valid && same(peers[j].record.mac, r->mac)) duplicate = true;
        if (!duplicate && (master || i == 0)) {
            peers[i].record = *r; radio_peer(r->mac, r->key);
            kms_radio_channel_peer(r->slot, r->mac, r->key);
        }
    }
}
static void release_local(void)
{
    guard.active = false;
    if (!master) {
        memset(&text_rx, 0, sizeof(text_rx)); text_receipt.pending = false;
    }
    if (clear_output) clear_output();
}
static bool delivery_send(void *ctx, const kms_packet_t *p)
{
    (void)ctx;
    bool submitted = p->slot && p->slot <= KMS_SLOTS && peers[p->slot - 1].record.valid &&
                     send_packet(peers[p->slot - 1].record.mac, p);
#if CONFIG_IDF_TARGET_ESP32S3
    if (submitted && sender.retransmission && sender.pending_traced &&
        (p->type == KMS_INPUT || p->type == KMS_INPUT_BATCH || p->type == KMS_ACTIVATE))
        kms_channel_recovery_input_retry(p->slot);
#endif
    return submitted;
}
void kms_link_set_key_trace(kms_key_trace_fn trace) { if (master) sender.trace = trace; }
static bool receipt_send(void *ctx, const kms_packet_t *p)
{
    (void)ctx;
    return peers[0].record.valid && send_packet(peers[0].record.mac, p);
}
static void start_error(uint32_t now)
{
    error_active = true; error_until = now + KMS_LED_ERROR_MS; kms_board_start_error(now);
}
static void radio_trace(const char *reason)
{
    portENTER_CRITICAL(&tx_lock);
    radio_tx_state_t tx = radio_tx; bool busy = tx_busy;
    portEXIT_CRITICAL(&tx_lock);
    uint32_t now = kms_now();
    ESP_LOGI(TAG, "KMSRADIO %s slot=%u type=%u sequence=%" PRIu32 " generation=%" PRIu32
        " buffered=%u oldest=%" PRIu32 "ms ack-age=%" PRIu32 "ms peer-age=%" PRIu32 "ms"
        " TX peer=" MACSTR " bytes=%" PRIu32 " submitted=%" PRIu32 " completed=%" PRIu32 " failed=%" PRIu32 " rejected=%" PRIu32
        " unexpected=%" PRIu32 " busy=%s age=%" PRIu32 "ms max=%" PRIu32
        "ms recoveries=%" PRIu32 " blocked=%s last-error=%" PRIu32,
        reason, master ? sender.slot : 0, master && sender.has_pending ? sender.pending.type : 0,
        master && sender.has_pending ? sender.pending.sequence : 0, master ? sender.generation : 0, master ? sender.count : 0,
        master && sender.count ? now - sender.buffer[sender.head].at : 0,
        master && sender.has_pending && sender.started ? now - sender.first : 0, master ? now - sender.seen : 0,
        MAC2STR(tx.mac), tx.bytes, tx.submitted, tx.completed, tx.failed, tx.rejected, tx.unexpected, busy ? "yes" : "no",
        busy ? now - tx.started : 0, tx.max_ms, tx.recoveries, tx.blocked ? "yes" : "no", tx.last_error);
    kms_clipboard_diagnostics_t clipboard; kms_clipboard_diagnostics(&clipboard);
    ESP_LOGI(TAG, "KMSRADIO ACK slot=%u count=%" PRIu32 " rtt=%" PRIu32 "ms max=%" PRIu32
        "ms dispatch=%" PRIu32 "ms dispatch-max=%" PRIu32 "ms",
        master ? sender.slot : 0, master ? sender.ack_count : 0, master ? sender.ack_rtt_ms : 0,
        master ? sender.ack_max_ms : 0, radio_rx_last_ms, radio_rx_max_ms);
    ESP_LOGI(TAG, "KMSCLIPBOARD usb=%" PRIu32 " radio=%" PRIu32 " unauthorized=%" PRIu32
        " relayed=%" PRIu32 " delivered=%" PRIu32 " drops=%" PRIu32
        " queues=%u/%u/%u deferred=%" PRIu32,
        clipboard.usb_received, clipboard.radio_received, clipboard.unauthorized, clipboard.relayed,
        clipboard.delivered, clipboard.queue_drops, clipboard.commands, clipboard.incoming, clipboard.outgoing,
        clipboard_deferred);
    for (unsigned i = 0; i < KMS_SLOTS; ++i) if (peers[i].record.valid) {
        radio_class_t classes[RADIO_KINDS];
        portENTER_CRITICAL(&tx_lock); memcpy(classes, radio_peers[i], sizeof(classes)); portEXIT_CRITICAL(&tx_lock);
        ESP_LOGI(TAG, "KMSRADIO PEER slot=%u input=%" PRIu32 "/%" PRIu32 "/%" PRIu32
            " desktop=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " clipboard=%" PRIu32 "/%" PRIu32 "/%" PRIu32
            " other=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " (completed/failed/max-ms)", i + 1,
            classes[0].completed, classes[0].failed, classes[0].max_ms,
            classes[1].completed, classes[1].failed, classes[1].max_ms,
            classes[2].completed, classes[2].failed, classes[2].max_ms,
            classes[3].completed, classes[3].failed, classes[3].max_ms);
    }
    portENTER_CRITICAL(&tx_lock); unsigned end = radio_slow_next; portEXIT_CRITICAL(&tx_lock);
    for (unsigned n = end > 4 ? end - 4 : 0; n < end; ++n) {
        portENTER_CRITICAL(&tx_lock); radio_slow_t slow = radio_slow[n % RADIO_SLOW_RECORDS]; portEXIT_CRITICAL(&tx_lock);
        ESP_LOGI(TAG, "KMSRADIO SLOW at=%" PRIu32 " slot=%u kind=%u op=%u sequence=%" PRIu32
            " bytes=%u ms=%" PRIu32 " failed=%u", slow.at, slow.slot, slow.kind, slow.op,
            slow.sequence, slow.bytes, slow.ms, slow.failed);
    }
}
static void delivery_event(void *ctx, enum kms_event event, unsigned slot, uint32_t detail)
{
    if (master && event == KMS_EVENT_ACK_TIMEOUT && slot && slot <= KMS_SLOTS && peers[slot - 1].record.valid)
        kms_receiver_recovery_fault(&receiver_recovery[slot - 1], peers[slot - 1].boot,
            sender.slot == slot, KMS_RECOVERY_ACK, kms_now());
    if (event == KMS_EVENT_ACK_TIMEOUT &&
        (!radio_trace_seen || kms_time_elapsed(kms_now(), last_radio_trace, 5000u))) {
        radio_trace_seen = true; last_radio_trace = kms_now(); radio_trace("ack-timeout");
    }
    (void)ctx; kms_diagnostics_event(event, slot, detail);
    kms_test_record(event == KMS_EVENT_SELECTED ? KMS_T_SELECT : KMS_T_FAULT, event, slot, detail, NULL, 0);
    if (event == KMS_EVENT_RECOVERED) {
        recovery_warning_started = false;
        ESP_LOGI(TAG, "Computer %u recovered after %" PRIu32 "ms", slot, detail);
    } else {
        // Later fault reasons and discarded input belong to the same outage.
        if (!recovery_warning_started) { start_error(kms_now()); recovery_warning_started = true; }
        memset(&led_feedback, 0, sizeof(led_feedback));
    }
}
static void fail_link(enum kms_event reason)
{
    kms_test_record(KMS_T_FAULT, reason, 0, 0, NULL, 0);
    if (master) { kms_delivery_fault(&sender, reason, kms_now()); return; }
    if (reason == KMS_EVENT_RX_OVERFLOW) kms_slave_diagnostics_event(KMS_S_RX_OVERFLOW, 0);
    if (reason == KMS_EVENT_USB_STALL) kms_slave_diagnostics_event(KMS_S_USB_STALL, 0);
    ESP_LOGW(TAG, "%s; releasing input", kms_event_name(reason));
    fault = true;
    start_error(kms_now());
    release_local();
}
void kms_link_emit(void *ctx, const kms_state_t *s, int32_t x, int32_t y, int32_t wheel, int32_t pan)
{
    if (!master) return;
#ifdef CONFIG_KMS_TEST_SUPPORT
    kms_sample_t traced = {.state = *s, .x = x, .y = y, .wheel = wheel, .pan = pan};
    kms_test_sample(KMS_T_ROUTED, sender.slot, &traced);
#endif
    (void)ctx;
    desktop_idle = !s->modifiers && !s->buttons && !s->system;
    for (unsigned i = 0; i < KMS_KEY_BYTES; ++i) desktop_idle &= !s->keys[i];
    for (unsigned i = 0; i < KMS_CONSUMERS; ++i) desktop_idle &= !s->consumer[i];
    kms_desktop_motion(x, y);
    kms_delivery_input(&sender, s, x, y, wheel, pan, kms_now());
    kms_diagnostics_buffer(sender.count);
    // USB routing runs after this iteration's link tick. Do not sleep again
    // before the newly queued input gets its first submission opportunity.
    if (sender.count) kms_app_wake();
}
bool kms_link_select(void *ctx, unsigned slot)
{
    if (!master) return false;
    kms_test_record(KMS_T_SELECT, slot, sender.slot, sender.generation, NULL, 0);
    (void)ctx;
    if (!slot || slot > KMS_SLOTS || pairing.open || kms_remote_master_updating()) return false;
    peer_t *p = &peers[slot - 1];
#if CONFIG_IDF_TARGET_ESP32S3
    if (kms_radio_channel_trial_active() && p->record.valid) {
        // The user owns the next route. Apply it when rollback has a fresh contact.
        channel_route.slot = slot;
        channel_route.epoch = ++route_epoch;
        channel_route.manual = true;
        channel_route.waiting = false;
        kms_channel_recovery_cancel();
        return true;
    }
#endif
    if (kms_radio_channel_changing()) return false;
    ++route_epoch;
    // Selection depends on dongle reachability; USB readiness only gates delivery.
    if (!p->record.valid || !p->boot || kms_time_elapsed(kms_now(), p->seen, KMS_PEER_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Computer %u is unavailable", slot);
        kms_diagnostics_event(KMS_EVENT_SELECT_UNAVAILABLE, slot, 0);
        start_error(kms_now()); return false;
    }
    if (sender.count) kms_diagnostics_event(KMS_EVENT_REPLAY_DROPPED, sender.slot, sender.count);
    last_selection = kms_now();
    kms_delivery_select(&sender, slot, p->boot, last_selection);
    sender.batch_limit = p->batch_limit;
    recovery_warning_started = false;
    memset(&led_feedback, 0, sizeof(led_feedback)); initial_selected = true;
    kms_diagnostics_event(KMS_EVENT_SELECTED, slot, 0);
    kms_app_wake();
    ESP_LOGI(TAG, "Selected computer %u", slot); return true;
}
bool kms_link_desktop_available(unsigned slot)
{
    if (!master || !slot || slot > KMS_SLOTS) return false;
    const peer_t *p = &peers[slot - 1];
    return p->record.valid && p->boot && (p->flags & KMS_FLAG_USB_READY) &&
        !kms_time_elapsed(kms_now(), p->seen, KMS_PEER_TIMEOUT_MS);
}
static void pause_route(void)
{
    ++route_epoch;
    if (sender.slot) paused_slot = sender.slot;
    kms_delivery_stop(&sender); initial_selected = true; fault = true;
    memset(&led_feedback, 0, sizeof(led_feedback));
    printf("KMSROUTE PAUSED %u\n", paused_slot);
}
void kms_link_pause(void)
{
    if (!master) return;
#if CONFIG_IDF_TARGET_ESP32S3
    channel_route.slot = 0;
    kms_channel_recovery_cancel();
#endif
    pause_route();
}
bool kms_link_has_pair(unsigned slot)
{ return master && slot && slot <= KMS_SLOTS && peers[slot - 1].record.valid; }
const char *kms_link_unpair(unsigned slot)
{
    if (!master || !slot || slot > KMS_SLOTS) return "INVALID";
    if (pairing.open || kms_remote_master_updating() || kms_radio_channel_changing()) return "BUSY";
    peer_t *peer = &peers[slot - 1];
    if (!peer->record.valid) return "NOT_PAIRED";
    ++route_epoch;
    if (sender.slot == slot) {
        kms_link_pause();
        recovery_warning_started = false;
    }
    if (paused_slot == slot) paused_slot = 0;
    if (sender.releasing && sender.release.slot == slot) sender.releasing = false;
    esp_now_del_peer(peer->record.mac);
    memset(peer, 0, sizeof(*peer));
    memset(&jiggler_peers[slot - 1], 0, sizeof(jiggler_peers[slot - 1]));
    kms_radio_channel_forget(slot);
    kms_remote_peer(slot, false, 0, 0, 0, peer->record.mac);
    save();
    return NULL;
}
void kms_link_clear_input(void)
{
    memset(&text_tx, 0, sizeof(text_tx));
    if (!master || !sender.slot) return;
    unsigned slot = sender.slot;
    if (sender.count) kms_diagnostics_event(KMS_EVENT_REPLAY_DROPPED, slot, sender.count);
    kms_delivery_select(&sender, slot, sender.receiver, kms_now());
    fault = true;
}
void kms_link_resume(void)
{
    if (!master) return;
#if CONFIG_IDF_TARGET_ESP32S3
    if (kms_radio_channel_trial_active()) {
        if (paused_slot) (void)kms_link_select(NULL, paused_slot);
        return;
    }
#endif
    ++route_epoch;
    if (kms_remote_master_updating()) { printf("KMSROUTE ERROR update in progress\n"); return; }
    if (sender.slot || !paused_slot || kms_link_select(NULL, paused_slot)) {
        printf("KMSROUTE RESUMED %u\n", sender.slot); paused_slot = 0; fault = true;
    } else printf("KMSROUTE ERROR %u\n", paused_slot);
}
#if CONFIG_IDF_TARGET_ESP32S3
void kms_link_channel_recovery_pause(void)
{
    channel_route.slot = sender.slot;
    pause_route();
    channel_route.epoch = route_epoch;
    channel_route.manual = channel_route.waiting = false;
}
void kms_link_channel_recovery_disown(void)
{
    if (!channel_route.manual) channel_route.slot = 0;
}
void kms_link_channel_recovery_finish(bool restore)
{
    if (!restore) channel_route.slot = 0;
    channel_route.waiting = true;
    channel_route.after = kms_now();
}
static void channel_restore_route(uint32_t now)
{
    unsigned slot = channel_route.slot;
    if (!slot || !channel_route.waiting) return;
    if (channel_route.epoch != route_epoch || sender.slot) { channel_route.slot = 0; return; }
    if (pairing.open || kms_remote_transfer_busy() || kms_radio_channel_changing() || kms_test_mode_active()) return;
    const peer_t *p = &peers[slot - 1];
    if (!p->record.valid || !p->boot || (int32_t)(p->seen - channel_route.after) < 0 ||
        now - p->seen >= KMS_PEER_TIMEOUT_MS || (!channel_route.manual && !(p->flags & KMS_FLAG_USB_READY))) return;
    channel_route.slot = 0;
    if (kms_link_select(NULL, slot)) {
        paused_slot = 0;
        fault = true;
        printf("KMSROUTE RESUMED %u\n", slot);
    }
}
#endif
bool kms_link_receiver_restart_in_progress(void)
{ return master && (recovery_owner || kms_remote_master_restart_pending()); }
void kms_link_set_usb_suspended(bool suspended) { usb_suspended = suspended; }
bool kms_link_receiver_host_available(void)
{
    portENTER_CRITICAL(&rx_lock);
    bool available = receiver_host_available;
    portEXIT_CRITICAL(&rx_lock);
    return available;
}
static void pair_close(void)
{
    if (pairing.candidate && !pairing.complete && esp_now_is_peer_exist(pairing.record.mac))
        esp_now_del_peer(pairing.record.mac);
    // Restore the previous assignment after an aborted enrollment.
    if (!pairing.complete) for (unsigned i = 0; i < KMS_SLOTS; ++i)
        if (peers[i].record.valid) radio_peer(peers[i].record.mac, peers[i].record.key);
    kms_exchange_end(&pairing.exchange);
    memset(&pairing, 0, sizeof(pairing));
}
void kms_link_pair(void *ctx, unsigned slot)
{
    if (kms_remote_master_updating() || kms_radio_channel_changing()) return;
    (void)ctx;
    if (master && (!slot || slot > KMS_SLOTS)) return;
    ++route_epoch;
    pair_close();
    if (!master) receipt.pending = false;
    if (master) {
        if (sender.count) kms_diagnostics_event(KMS_EVENT_REPLAY_DROPPED, sender.slot, sender.count);
        kms_delivery_stop(&sender);
    }
    recovery_warning_started = false; fault = true; release_local();
    memset(&led_feedback, 0, sizeof(led_feedback));
    if (!kms_exchange_start(&pairing.exchange)) { ESP_LOGE(TAG, "Cannot generate enrollment key"); return; }
    pairing.open = true; pairing.slot = master ? slot : 0;
    pairing.until = kms_now() + KMS_PAIR_WINDOW_MS;
    esp_fill_random(pairing.nonce, sizeof(pairing.nonce));
    ESP_LOGI(TAG, "Pairing window open (%u ms), slot %u", KMS_PAIR_WINDOW_MS, slot);
}
static void pair_message(unsigned type)
{
    kms_packet_t p = {.type = type, .slot = pairing.slot, .session = boot_id, .receiver = pairing.remote_boot};
    memcpy(p.nonce, pairing.nonce, sizeof(p.nonce));
    if (type == KMS_PAIR_REQUEST || type == KMS_PAIR_OFFER)
        memcpy(p.public_key, pairing.exchange.public_key, sizeof(p.public_key));
    else if (!kms_pair_proof(pairing.record.key, pairing.nonce, pairing.slot, type == KMS_PAIR_DONE, p.public_key)) return;
    send_packet(type == KMS_PAIR_REQUEST ? broadcast : pairing.record.mac, &p);
    pairing.last = kms_now();
}
static bool proof_valid(const kms_packet_t *p, bool done)
{
    uint8_t proof[KMS_NONCE_BYTES];
    if (!kms_pair_proof(pairing.record.key, pairing.nonce, pairing.slot, done, proof)) return false;
    uint8_t difference = 0;
    for (unsigned i = 0; i < sizeof(proof); ++i) difference |= proof[i] ^ p->public_key[i];
    return !difference;
}
static void pair_receive(const received_t *r)
{
    const kms_packet_t *p = &r->packet;
    if (!pairing.open) return;
    if (master && p->type == KMS_PAIR_REQUEST && !pairing.candidate && p->session) {
        // Reassigning an already paired receiver to another slot is deliberately explicit.
        for (unsigned i = 0; i < KMS_SLOTS; ++i)
            if (peers[i].record.valid && same(peers[i].record.mac, r->mac) && i + 1 != pairing.slot) {
                ESP_LOGW(TAG, "Receiver already belongs to slot %u", i + 1); return;
            }
        pairing.record = (record_t){.valid = 1, .slot = pairing.slot};
        memcpy(pairing.record.mac, r->mac, sizeof(r->mac));
        memcpy(pairing.nonce, p->nonce, sizeof(pairing.nonce));
        pairing.remote_boot = p->session;
        if (!kms_exchange_key(&pairing.exchange, p->public_key, pairing.nonce, own_mac, r->mac,
                              pairing.slot, true, pairing.record.key)) return;
        record_t *old = &peers[pairing.slot - 1].record;
        if (old->valid && !same(old->mac, r->mac)) esp_now_del_peer(old->mac);
        radio_peer(r->mac, NULL); pairing.candidate = true;
        pair_message(KMS_PAIR_OFFER); pairing.key_at = kms_now() + KMS_PAIR_KEY_DELAY_MS;
    } else if (!master && p->type == KMS_PAIR_OFFER && !pairing.candidate && p->slot &&
               p->receiver == boot_id && !memcmp(p->nonce, pairing.nonce, sizeof(p->nonce))) {
        pairing.slot = p->slot; pairing.remote_boot = p->session;
        pairing.record = (record_t){.valid = 1, .slot = p->slot};
        memcpy(pairing.record.mac, r->mac, sizeof(r->mac));
        if (!kms_exchange_key(&pairing.exchange, p->public_key, pairing.nonce, r->mac, own_mac,
                              p->slot, false, pairing.record.key)) return;
        radio_peer(r->mac, pairing.record.key); pairing.candidate = pairing.encrypted = true;
        pair_message(KMS_PAIR_CONFIRM);
    } else if (pairing.candidate && same(pairing.record.mac, r->mac) && p->slot == pairing.slot &&
               p->session == pairing.remote_boot && p->receiver == boot_id &&
               !memcmp(p->nonce, pairing.nonce, sizeof(p->nonce))) {
        if (master && p->type == KMS_PAIR_CONFIRM && pairing.encrypted && proof_valid(p, false)) {
            if (!pairing.complete) {
                peers[pairing.slot - 1] = (peer_t){.record = pairing.record}; save(); pairing.complete = true;
                kms_radio_channel_peer(pairing.slot, pairing.record.mac, pairing.record.key);
                ESP_LOGI(TAG, "Paired computer %u", pairing.slot);
            }
            pair_message(KMS_PAIR_DONE);
        } else if (!master && p->type == KMS_PAIR_DONE && proof_valid(p, true)) {
            if (peers[0].record.valid && !same(peers[0].record.mac, r->mac)) esp_now_del_peer(peers[0].record.mac);
            peers[0] = (peer_t){.record = pairing.record}; save(); pairing.complete = true;
            kms_radio_channel_peer(pairing.slot, pairing.record.mac, pairing.record.key);
            kms_radio_channel_paired();
            ESP_LOGI(TAG, "Paired as computer %u", pairing.slot); pair_close();
        }
    }
}
static void received(const received_t *r, uint32_t received_at)
{
    const kms_packet_t *p = &r->packet;
    if (p->type >= KMS_PAIR_REQUEST && p->type <= KMS_PAIR_DONE) { pair_receive(r); return; }
    if (master) {
        if (!p->slot) return;
        peer_t *peer = &peers[p->slot - 1];
        if (!peer->record.valid || !same(peer->record.mac, r->mac)) return;
        peer->rssi = r->rssi;
        kms_diagnostics_radio(false, false, false);
        if (p->type == KMS_TEXT_ACK) {
            if (kms_text_tx_receive(&text_tx, p)) sender.seen = kms_now();
            return;
        }
        if (p->type == KMS_STATUS) {
            kms_jiggler_sync_status(&jiggler_peers[p->slot - 1], p, boot_id, jiggler_seconds, jiggler_distance);
            peer->boot = p->session; peer->seen = received_at; peer->flags = p->flags;
            peer->suspended = (p->public_key[6] & KMS_STATUS_USB_SUSPENDED) != 0;
            peer->batch_limit = (uint8_t)kms_batch_limit(p);
            kms_delivery_receive(&sender, p, kms_now());
            if (pairing.complete && pairing.slot == p->slot) pair_close();
        } else if (p->type == KMS_ACK) kms_delivery_receive(&sender, p, kms_now());
        if (p->type == KMS_ACK || p->type == KMS_STATUS)
            kms_test_record(KMS_T_LINK_HANDLED, p->sequence, p->type, sender.count, NULL, 0);
        if (sender.slot == p->slot && !sender.recovering)
            kms_led_feedback_accept(&led_feedback, p, boot_id, peer->boot, sender.slot, sender.generation);
    } else {
        record_t *peer = &peers[0].record;
        if (!peer->valid || !same(peer->mac, r->mac) || p->slot != peer->slot || p->receiver != boot_id) return;
        if (p->type == KMS_JIGGLER) {
            if (kms_remote_restart_pending()) return;
            if ((!guard.active || p->session == guard.session) && kms_jiggler_configure(&jiggler, p, kms_now()))
                kms_jiggler_store_update(&jiggler);
            return;
        }
        if (!ready && p->type != KMS_RELEASE) return;
        if (p->type == KMS_TEXT) {
            if (kms_text_valid(p) && guard.active && p->session == guard.session && p->generation == guard.generation)
                kms_radio_channel_contact();
            bool cancel;
            if (kms_text_rx_receive(&text_rx, p, &guard, kms_now(), &cancel)) {
                kms_text_ack(&p->single, &text_receipt.packet); text_receipt.pending = true;
            }
            if (cancel && clear_output) clear_output();
            return;
        }
        kms_slave_diagnostics_radio(r->rssi, uxQueueMessagesWaiting(incoming) + 1);
        kms_sample_t samples[KMS_INPUT_BATCH_MAX]; unsigned count; bool apply;
        kms_receiver_guard_t next = guard;
        if (!kms_receiver_accept_many(&next, p, kms_now(), samples, &count, &apply)) {
            kms_test_record(KMS_T_REJECTED, p->sequence, p->generation, p->type, NULL, 0);
            kms_slave_diagnostics_add(KMS_S_REJECTED, 1); return;
        }
        kms_radio_channel_contact();
        if (apply) for (unsigned i = 0; i < count; ++i) kms_test_sample(KMS_T_ACCEPTED, p->sequence, &samples[i]);
        if (apply && (p->type == KMS_ACTIVATE || p->type == KMS_RELEASE)) {
            memset(&text_rx, 0, sizeof(text_rx)); text_receipt.pending = false;
            if (clear_output) clear_output();
        }
        if (apply && deliver && !deliver(samples, count)) { release_local(); return; }
        if (!apply) kms_test_record(KMS_T_ACCEPTED, p->sequence, p->generation, 0, NULL, 0);
        kms_slave_diagnostics_add(apply ? KMS_S_ACCEPTED : KMS_S_DUPLICATES, 1);
        guard = next;
        if (apply) for (unsigned i = 0; i < count; ++i) kms_desktop_input(&samples[i]);
        // The transmitter may still be sending status/management. Keep the
        // accepted sequence until it can be acknowledged ahead of background
        // traffic. Stop-and-wait input needs at most one pending receipt.
        kms_ack_prepare(&receipt, p);
    }
}
void kms_link_set_boot_slot(unsigned slot)
{
    if (slot <= KMS_SLOTS) boot_slot = slot;
}
void kms_link_set_jiggler_settings(uint16_t seconds, uint8_t distance)
{
    if (!seconds || !distance || distance > KMS_JIGGLER_MAX_DISTANCE ||
        (seconds == jiggler_seconds && distance == jiggler_distance)) return;
    jiggler_seconds = seconds; jiggler_distance = distance;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) if (jiggler_peers[i].known)
        kms_jiggler_sync_request(&jiggler_peers[i], jiggler_peers[i].enabled, seconds,
            jiggler_peers[i].distance_supported ? distance : 1, jiggler_peers[i].mode);
}
bool kms_link_toggle_jiggler(void *ctx)
{
    (void)ctx;
    kms_jiggler_status_t status;
    return !kms_link_jiggler_get(sender.slot, &status) && !kms_link_jiggler_set(sender.slot, !status.enabled);
}
bool kms_link_cycle_jiggler_mode(void *ctx)
{
    (void)ctx;
    kms_jiggler_status_t status;
    return !kms_link_jiggler_get(sender.slot, &status) &&
        !kms_link_jiggler_set_mode(sender.slot, (status.mode + 1) % KMS_JIGGLER_MODE_COUNT);
}
const char *kms_link_jiggler_get(unsigned slot, kms_jiggler_status_t *out)
{
    if (!master || !slot || slot > KMS_SLOTS || !peers[slot - 1].record.valid) return "INVALID";
    peer_t *peer = &peers[slot - 1]; kms_jiggler_sync_t *s = &jiggler_peers[slot - 1];
    if (!peer->boot || kms_time_elapsed(kms_now(), peer->seen, KMS_PEER_TIMEOUT_MS)) return "OFFLINE";
    if (!s->known) return "UNSUPPORTED";
    *out = (kms_jiggler_status_t){.boot = s->boot, .sequence = s->sequence, .seconds = s->interval_seconds,
        .distance = s->distance, .mode = s->mode, .modes_supported = s->modes_supported,
        .enabled = s->enabled, .pending = s->pending};
    return NULL;
}
const char *kms_link_jiggler_set(unsigned slot, bool enabled)
{
    kms_jiggler_status_t status;
    const char *error = kms_link_jiggler_get(slot, &status);
    if (error) return error;
    if (pairing.open || kms_remote_master_updating() || kms_radio_channel_changing()) return "BUSY";
    bool supports_distance = jiggler_peers[slot - 1].distance_supported;
    if (enabled && jiggler_distance != 1 && !supports_distance) return "UPGRADE_RECEIVER";
    uint8_t distance = supports_distance ? jiggler_distance : 1;
    // Absolute requests are idempotent, including retries from the web console.
    if (status.enabled != enabled || status.seconds != jiggler_seconds || status.distance != distance)
        kms_jiggler_sync_request(&jiggler_peers[slot - 1], enabled, jiggler_seconds, distance, status.mode);
    ESP_LOGI(TAG, "Mouse jiggler dongle %u: %s requested, interval %u seconds, distance %u counts", slot, enabled ? "on" : "off", jiggler_seconds, distance);
    return NULL;
}
const char *kms_link_jiggler_set_mode(unsigned slot, unsigned mode)
{
    if (mode >= KMS_JIGGLER_MODE_COUNT) return "INVALID";
    kms_jiggler_status_t status;
    const char *error = kms_link_jiggler_get(slot, &status);
    if (error) return error;
    if (pairing.open || kms_remote_master_updating() || kms_radio_channel_changing()) return "BUSY";
    if (!status.modes_supported) return "UPGRADE_RECEIVER";
    if (status.mode != mode)
        kms_jiggler_sync_request(&jiggler_peers[slot - 1], status.enabled, status.seconds, status.distance, mode);
    ESP_LOGI(TAG, "Mouse jiggler dongle %u: mode %s requested", slot, kms_jiggler_mode_name(mode));
    return NULL;
}
bool kms_link_jiggler_motion(kms_jiggler_motion_t *out) { return !master && kms_jiggler_motion(&jiggler, kms_now(), out); }
void kms_link_jiggler_moved(void) { kms_jiggler_moved(&jiggler, kms_now()); }
void kms_link_jiggler_activity(void) { kms_jiggler_activity(&jiggler, kms_now()); }
static struct { uint32_t sequence, kind, at, boot, reasons, attempts; } recovery_notices[KMS_SLOTS];
static uint32_t recovery_notice_sequence;
static uint32_t recovery_notice_reasons[KMS_SLOTS];
static void recovery_notice(unsigned index, unsigned kind, uint32_t now)
{
    if (!++recovery_notice_sequence) ++recovery_notice_sequence;
    recovery_notices[index].sequence = recovery_notice_sequence;
    recovery_notices[index].kind = kind; recovery_notices[index].at = now;
    recovery_notices[index].boot = peers[index].boot;
    recovery_notices[index].reasons = recovery_notice_reasons[index];
    recovery_notices[index].attempts = receiver_recovery[index].attempts;
}
bool kms_link_recovery_status(unsigned slot, uint8_t out[48])
{
    if (!master || !slot || slot > KMS_SLOTS) return false;
    unsigned i = slot - 1;
    const uint32_t values[] = {1, boot_id, recovery_notices[i].sequence, recovery_notices[i].kind, slot,
        recovery_notices[i].at, recovery_notices[i].sequence ? kms_now() - recovery_notices[i].at : 0,
        recovery_notices[i].boot, recovery_notices[i].reasons, recovery_notices[i].attempts, sender.slot, 0};
    for (unsigned n = 0; n < 12; ++n) kms_put_u32(out + n * 4, values[n]);
    return true;
}
static void recovery_restore_route(void)
{
    if (!recovery_resume_slot) return;
    if (route_epoch != recovery_resume_epoch || sender.slot || paused_slot != recovery_resume_slot) {
        recovery_resume_slot = 0; return;
    }
    if (pairing.open || kms_remote_transfer_busy() || kms_radio_channel_changing()) return;
    peer_t *peer = &peers[recovery_resume_slot - 1];
    bool confirmed;
    if (!peer->record.valid || !(peer->flags & KMS_FLAG_USB_READY) ||
        kms_time_elapsed(kms_now(), peer->seen, KMS_PEER_TIMEOUT_MS) ||
        !kms_remote_recovery_supported(recovery_resume_slot, peer->boot, &confirmed) || !confirmed) return;
    kms_link_resume(); recovery_resume_slot = 0;
}
static void receiver_recovery_tick(uint32_t now)
{
    if (!master) return;
    bool channel_hold = kms_radio_channel_recovering();
    bool manual_restart = kms_remote_master_restart_pending();
#if CONFIG_IDF_TARGET_ESP32S3
    bool master_stalled = processing_fault_seen && now - processing_fault_at < 1500u;
#else
    bool master_stalled = false;
#endif
    if (!recovery_owner) recovery_restore_route();
    kms_recovery_result_t result;
    if (kms_remote_take_recovery_result(&result) && recovery_owner == result.slot &&
        receiver_recovery[result.slot - 1].restart_boot == result.boot) {
        if (result.status != KMS_M_OK && result.status != KMS_M_TIMEOUT && result.status != KMS_M_WRONG_BOOT) {
            if (result.status != KMS_M_BUSY) recovery_notice(result.slot - 1, 2, now);
            receiver_recovery[result.slot - 1].waiting = false; recovery_owner = 0;
            ESP_LOGW(TAG, "KMSRECOVERY deferred slot=%u result=%s", result.slot, kms_management_result_name(result.status));
            recovery_restore_route();
        }
    }
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        peer_t *peer = &peers[i]; kms_receiver_recovery_t *policy = &receiver_recovery[i];
        if (!peer->record.valid) continue;
        portENTER_CRITICAL(&tx_lock);
        uint32_t errors = recovery_errors[i], reasons = recovery_reasons[i]; recovery_reasons[i] = 0;
        bool driver_blocked = radio_tx.blocked ||
            (radio_rejection_seen && !kms_time_elapsed(now, radio_rejected_at, KMS_RECOVERY_QUIET_MS));
        portEXIT_CRITICAL(&tx_lock);
        if (!recovery_initialized[i] || memcmp(recovery_macs[i], peer->record.mac, 6)) {
            kms_recovery_budget_t budget; kms_remote_recovery_budget(i + 1, peer->record.mac, &budget);
            memset(policy, 0, sizeof(*policy)); kms_receiver_recovery_restore(policy, &budget, now);
            memcpy(recovery_macs[i], peer->record.mac, 6); recovery_initialized[i] = true;
            recovery_seen_errors[i] = errors;
            if (recovery_owner == i + 1) recovery_owner = 0;
        }
        bool confirmed = false, supported = kms_remote_recovery_supported(i + 1, peer->boot, &confirmed);
        bool online = peer->boot && !kms_time_elapsed(now, peer->seen, KMS_PEER_TIMEOUT_MS) &&
            (peer->flags & KMS_FLAG_USB_READY) && !peer->suspended;
        if (channel_hold || master_stalled || driver_blocked) {
            recovery_seen_errors[i] = errors;
            recovery_online[i] = policy->fault_seen = false;
            policy->faults = policy->reasons = 0;
            continue;
        }
        if (!online || !recovery_online[i]) {
            policy->faults = policy->reasons = 0; policy->fault_seen = false;
            if (online) policy->boot_at = now;
        }
        recovery_online[i] = online;
        if (policy->waiting && online && kms_receiver_recovery_confirm(policy, peer->boot, confirmed, now)) {
            recovery_notice(i, 1, now);
            ESP_LOGI(TAG, "KMSRECOVERY confirmed slot=%u boot=%08" PRIx32 " attempts=%u level=%u",
                i + 1, peer->boot, policy->attempts, policy->level);
            if (recovery_owner == i + 1) { recovery_owner = 0; recovery_restore_route(); }
        } else if (kms_receiver_recovery_expired(policy, now)) {
            recovery_notice(i, 2, now);
            ESP_LOGW(TAG, "KMSRECOVERY boot not confirmed slot=%u; backing off; manual reset may be needed", i + 1);
            if (recovery_owner == i + 1) { recovery_owner = 0; recovery_resume_slot = 0; }
        }
        kms_receiver_recovery_boot(policy, peer->boot, now);
        if (errors != recovery_seen_errors[i]) {
            recovery_seen_errors[i] = errors;
            if (online) kms_receiver_recovery_fault(policy, peer->boot, sender.slot == i + 1, reasons, now);
        }
        if (online && confirmed) kms_receiver_recovery_healthy(policy, now);
        bool active = sender.slot == i + 1;
        bool blocked = recovery_owner || manual_restart || !kms_remote_recovery_enabled() || driver_blocked || !online || !supported || !confirmed ||
            pairing.open || kms_remote_transfer_busy() || kms_radio_channel_changing() || kms_test_mode_active() ||
            (recovery_global_wait && !kms_time_elapsed(now, recovery_global_at, 30000u));
        if (blocked || !kms_receiver_recovery_due(policy, active, now)) continue;
        unsigned faults = policy->faults, why = policy->reasons;
        recovery_notice_reasons[i] = why;
        kms_receiver_recovery_begin(policy, now);
        kms_recovery_budget_t budget = {.attempts = policy->attempts, .level = policy->level,
            .boot = peer->boot, .reasons = why}; memcpy(budget.mac, peer->record.mac, 6);
        if (active) {
            kms_link_pause(); recovery_resume_slot = i + 1; recovery_resume_epoch = route_epoch;
        }
        recovery_global_wait = true; recovery_global_at = now;
        ESP_LOGW(TAG, "KMSRECOVERY scheduled slot=%u boot=%08" PRIx32 " active=%u faults=%u reasons=0x%x"
            " attempt=%u cooldown_ms=%" PRIu32, i + 1, peer->boot, active, faults, why,
            policy->attempts, policy->next_at - now);
        if (kms_remote_recovery_request(i + 1, peer->boot, &budget, active)) recovery_owner = i + 1;
        else {
            policy->waiting = false; recovery_restore_route();
            ESP_LOGW(TAG, "KMSRECOVERY queue unavailable slot=%u; backing off", i + 1);
        }
    }
}
static void jiggler_tick(uint32_t now)
{
    if (pairing.open || kms_radio_channel_changing() || sender.has_pending || sender.count || sender.releasing) return;
    for (unsigned n = 0; n < KMS_SLOTS; ++n) {
        unsigned index = (jiggler_next + n) % KMS_SLOTS;
        kms_jiggler_sync_t *s = &jiggler_peers[index]; kms_packet_t p;
        if (!peers[index].record.valid || kms_time_elapsed(now, peers[index].seen, KMS_PEER_TIMEOUT_MS) ||
            !kms_jiggler_sync_packet(s, boot_id, index + 1, now, &p)) continue;
        if (send_packet(peers[index].record.mac, &p)) {
            s->sent = true; s->sent_at = now; jiggler_next = (index + 1) % KMS_SLOTS;
        }
        return;
    }
}
static void channel_release(void)
{
    release_local();
    if (!master) memset(&receipt, 0, sizeof(receipt));
    xQueueReset(incoming); xQueueReset(management_incoming); xQueueReset(channel_incoming);
}
static unsigned stream_space(void) { return uxQueueSpacesAvailable(management_incoming); }
static void stream_receive(const kms_management_frame_t *frame, const uint8_t *mac, int8_t rssi)
{
    management_received_t message = {.frame = *frame, .rssi = rssi};
    memcpy(message.mac, mac, sizeof(message.mac)); (void)xQueueSend(management_incoming, &message, 0);
}
static bool stream_peer(unsigned slot, uint8_t *mac, uint32_t *boot)
{
    if (!slot || slot > KMS_SLOTS) return false;
    peer_t *p = master ? &peers[slot - 1] : &peers[0];
    if (!p->record.valid || p->record.slot != slot) return false;
    memcpy(mac, p->record.mac, 6); *boot = p->boot; return true;
}
void kms_link_init(bool is_master, kms_deliver_fn callback, void (*clear)(void))
{
    kms_app_init();
    configASSERT(is_master == master);
    deliver = callback; clear_output = clear;
    // Reserve the replay capacity before the Wi-Fi driver can fragment the
    // internal fallback heap.
    kms_buffered_t *replay = NULL;
    if (master) {
        replay = kms_memory_calloc(KMS_RECOVERY_BUFFER_DEPTH, sizeof(*replay));
        ESP_ERROR_CHECK(replay ? ESP_OK : ESP_ERR_NO_MEM);
    }
    ESP_ERROR_CHECK(nvs_flash_init()); // Never erase an existing peer store to hide an initialization error.
    kms_test_mode_init(master);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&c));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_country_code(KMS_RADIO_COUNTRY, false));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(KMS_RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, own_mac));
    // ESP-NOW callbacks run on the Wi-Fi task. Queue only application copies;
    // the driver and its radio buffers retain their own memory policy.
    incoming = kms_memory_queue_create(KMS_RADIO_RX_DEPTH, sizeof(queued_input_t)); configASSERT(incoming);
    management_incoming = kms_memory_queue_create(KMS_MANAGEMENT_QUEUE_DEPTH, sizeof(management_received_t));
    configASSERT(management_incoming);
    channel_incoming = kms_memory_queue_create(8, sizeof(channel_received_t)); configASSERT(channel_incoming);
    ESP_ERROR_CHECK(esp_now_init()); ESP_ERROR_CHECK(esp_now_register_recv_cb(receive));
    ESP_ERROR_CHECK(esp_now_register_send_cb(sent));
    ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
    do { boot_id = esp_random(); } while (!boot_id);
    kms_radio_channel_init(master, boot_id, send_frame, channel_release);
    if (master) {
#if CONFIG_IDF_TARGET_ESP32S3
        kms_channel_recovery_init();
#endif
        kms_diagnostics_init(boot_id);
        kms_delivery_init(&sender, replay, boot_id, delivery_send, delivery_event, NULL);
    }
    radio_peer(broadcast, NULL); load();
    if (!master) kms_jiggler_store_load(&jiggler, kms_now());
    kms_stream_runtime_init();
    kms_stream_radio_init(boot_id, stream_space, stream_receive);
    kms_remote_init(master, boot_id);
    kms_desktop_init(master, boot_id);
    ESP_LOGI(TAG, "%s: " MACSTR ", channel %u", master ? "Master" : "Receiver", MAC2STR(own_mac), kms_radio_channel_get());
}
static bool route_starting(uint32_t now)
{
    // A desktop READY can select a route after this iteration's delivery tick.
    // Keep its reply and other background traffic behind release, activation
    // and the first queued input. Bound the priority window if a dongle fails.
    return master && !kms_time_elapsed(now, last_selection, KMS_RADIO_RETRY_WINDOW_MS) &&
        (sender.releasing || (sender.has_pending && sender.pending.type == KMS_ACTIVATE) || sender.count);
}
static bool take_management_frame(kms_management_frame_t *frame)
{ return kms_desktop_take_outgoing(frame) || kms_remote_take_outgoing(frame); }
static bool bulk_allowed(void)
{
    // A free driver slot can still be waiting for an application input receipt.
    if (master && (sender.has_pending || sender.releasing || sender.need_activate ||
                   sender.recovering || text_tx.pending)) { ++clipboard_deferred; return false; }
    if (!master && (receipt.pending || text_receipt.pending)) { ++clipboard_deferred; return false; }
    return true;
}
static void management_tick(bool usb_ready)
{
    kms_desktop_link_tick(master ? sender.slot : peers[0].record.slot,
        master ? boot_id : guard.session, master ? sender.generation : guard.generation,
        guard.active, usb_ready, desktop_idle && (!master || (!sender.recovering && !sender.need_activate && !sender.releasing)));
    if (master) for (unsigned slot = 1; slot <= KMS_SLOTS; ++slot) {
        peer_t *p = &peers[slot - 1];
        kms_remote_peer(slot, p->record.valid, p->boot, p->seen,
                        p->suspended ? p->flags & ~KMS_FLAG_USB_READY : p->flags, p->record.mac);
    }
    management_received_t message;
    for (unsigned i = 0; i < KMS_MANAGEMENT_QUEUE_DEPTH &&
         xQueueReceive(management_incoming, &message, 0) == pdTRUE; ++i) {
        kms_management_frame_t *f = &message.frame;
        if (pairing.open || !f->sender) continue;
        if (master) {
            peer_t *p = &peers[f->slot - 1];
            if (f->op == KMS_M_DESKTOP || f->op == KMS_M_CLIPBOARD) {
                kms_desktop_link_tick(sender.slot, boot_id, sender.generation, false, usb_ready,
                    desktop_idle && !sender.recovering && !sender.need_activate && !sender.releasing);
                if (p->record.valid && same(p->record.mac, message.mac) && p->boot == f->sender) {
                    if (f->op == KMS_M_CLIPBOARD) kms_clipboard_receive(f);
                    else kms_desktop_receive(f);
                }
                continue;
            }
            if (!p->record.valid || !same(p->record.mac, message.mac) || p->boot != f->sender ||
                (!(f->flags & KMS_M_SUMMARY) && f->target != boot_id) ||
                !(f->flags & (KMS_M_REPLY | KMS_M_SUMMARY))) continue;
        } else {
            record_t *p = &peers[0].record;
            if (!p->valid || p->slot != f->slot || !same(p->mac, message.mac) ||
                (f->flags & KMS_M_SUMMARY)) continue;
            radio_receive_timing(kms_now());
            kms_slave_diagnostics_radio(message.rssi, uxQueueMessagesWaiting(incoming));
            if (f->target == boot_id) kms_radio_channel_contact();
        }
        if (kms_stream_radio_control(f)) continue;
        if (master) kms_diagnostics_radio(false, false, false);
        if (f->op == KMS_M_DESKTOP) { kms_desktop_receive(f); continue; }
        if (f->op == KMS_M_CLIPBOARD) { kms_clipboard_receive(f); continue; }
        kms_remote_receive(f);
    }
    // Receiver app ticks start after TinyUSB installs; an unmounted/suspended PC
    // affects routing readiness, not initialization health for OTA confirmation.
    kms_remote_tick(master ? sender.slot : peers[0].record.slot, !master || usb_ready, guard.active);
    uint32_t now = kms_now();
    if (pairing.open || route_starting(now)) return;
    // Input is submitted first. Leave management queued until its transmitter
    // slot is free; its completion must use the same gate as every other frame.
    portENTER_CRITICAL(&tx_lock); bool busy = tx_busy; portEXIT_CRITICAL(&tx_lock);
    if (busy || (!master && (receipt.pending || text_receipt.pending))) return;
    kms_management_frame_t f;
    static kms_management_frame_t held;
    static bool holding;
    if (!kms_time_elapsed(now, last_management, KMS_MANAGEMENT_PACE_MS)) return;
    if (!holding) holding = take_management_frame(&held);
    if (!holding) {
        kms_stream_radio_tick(bulk_allowed(), now, send_frame, stream_peer); return;
    }
    f = held;
    record_t *peer = master ? &peers[f.slot - 1].record : &peers[0].record;
    if (!peer->valid || peer->slot != f.slot) { holding = false; return; }
    uint8_t wire[KMS_M_MAX_WIRE];
    size_t bytes = kms_management_encode(&f, wire, sizeof(wire));
    if (bytes && send_frame(peer->mac, wire, bytes)) {
        holding = false; if (master) kms_diagnostics_radio(true, false, false);
    } else if (f.op == KMS_M_RESTART) {
        // This separate queue contains automatic restarts only. Let its RPC
        // retry revalidate the current route, deadline and recovery policy.
        holding = false;
    }
    last_management = now;
}
void kms_link_tick(bool usb_ready)
{
    uint32_t tick_now = kms_now();
    uint32_t gap = radio_tick_at ? tick_now - radio_tick_at : 0;
    radio_tick_at = tick_now;
    if (gap > radio_tick_max) radio_tick_max = gap;
    if (gap >= 50u) {
#if CONFIG_IDF_TARGET_ESP32S3
        processing_fault_at = tick_now;
        processing_fault_seen = true;
#else
        kms_radio_channel_processing_fault(tick_now);
#endif
    }
    if (!master && tick_now > 5000u && gap >= 50u && !kms_ota_busy() && !pairing.open)
        kms_slave_diagnostics_fault(KMS_RF_MAIN_GAP, 0, gap, radio_tx.failed);
    radio_health_tick();
    if (!master && kms_ota_busy()) usb_ready = false;
    if (ready && !usb_ready && !master) release_local();
    ready = usb_ready;
    if (!master) {
        portENTER_CRITICAL(&rx_lock);
        receiver_host_available = ready && !usb_suspended;
        portEXIT_CRITICAL(&rx_lock);
    }
    channel_received_t channel_message;
    for (unsigned n = 0; n < 8 && xQueueReceive(channel_incoming, &channel_message, 0) == pdTRUE; ++n) {
        kms_radio_message_t response;
        if (kms_radio_channel_receive(channel_message.mac, channel_message.wire, sizeof(channel_message.wire),
                                      channel_message.received_at, &response)) {
#if CONFIG_IDF_TARGET_ESP32S3
            kms_channel_recovery_reply(&response, channel_message.received_at);
#endif
        }
    }
    portENTER_CRITICAL(&tx_lock); bool radio_busy = tx_busy; portEXIT_CRITICAL(&tx_lock);
    unsigned channel_before = kms_radio_channel_get();
    kms_radio_channel_tick(pairing.open, pairing.candidate, radio_busy);
    if (master && channel_before != kms_radio_channel_get()) {
        xQueueReset(incoming);
        xQueueReset(management_incoming);
        xQueueReset(channel_incoming);
    }
    portENTER_CRITICAL(&rx_lock); bool overflow = rx_overflow; rx_overflow = false; portEXIT_CRITICAL(&rx_lock);
    if (overflow) { xQueueReset(incoming); fail_link(KMS_EVENT_RX_OVERFLOW); }
    queued_input_t message; received_t r;
    for (unsigned n = 0; n < KMS_RADIO_RX_DEPTH && xQueueReceive(incoming, &message, 0) == pdTRUE; ++n) {
        if (!kms_wire_decode(&r.packet, message.wire, message.length)) continue;
        radio_rx_last_ms = kms_now() - message.received_at;
        if (radio_rx_last_ms > radio_rx_max_ms) radio_rx_max_ms = radio_rx_last_ms;
        if (!master && peers[0].record.valid && same(message.mac, peers[0].record.mac))
            radio_receive_timing(message.received_at);
        memcpy(r.mac, message.mac, sizeof(r.mac)); r.rssi = message.rssi;
        received(&r, message.received_at);
    }
    // Receive handlers timestamp events and may spend time in key exchange/NVS.
    // Sample after processing them rather than comparing their newer timestamps
    // with the clock at entry to this tick.
    uint32_t now = kms_now();
    if (pairing.open) {
        if ((int32_t)(now - pairing.until) >= 0) { ESP_LOGI(TAG, "Pairing window closed"); pair_close(); }
        else if (!master && !pairing.candidate && kms_time_elapsed(now, pairing.last, KMS_PAIR_RETRY_MS)) pair_message(KMS_PAIR_REQUEST);
        else if (!master && pairing.candidate && kms_time_elapsed(now, pairing.last, KMS_PAIR_RETRY_MS)) pair_message(KMS_PAIR_CONFIRM);
        else if (master && pairing.candidate && !pairing.encrypted && (int32_t)(now - pairing.key_at) >= 0) {
            radio_peer(pairing.record.mac, pairing.record.key); pairing.encrypted = true;
        } else if (master && pairing.candidate && !pairing.complete && kms_time_elapsed(now, pairing.last, KMS_PAIR_OFFER_RETRY_MS)) {
            radio_peer(pairing.record.mac, NULL); pairing.encrypted = false;
            pair_message(KMS_PAIR_OFFER); pairing.key_at = pairing.last + KMS_PAIR_KEY_DELAY_MS;
        }
    }
    now = kms_now();
    if (!master) {
        if (kms_receiver_expire(&guard, now)) {
            kms_slave_diagnostics_fault(KMS_RF_RX_GAP, 0, now - guard.last_seen, radio_tx.failed);
            kms_slave_diagnostics_event(KMS_S_LEASE_EXPIRED, 0); release_local();
        }
        receipt.packet.flags = (ready ? KMS_FLAG_USB_READY : 0) | (guard.active ? KMS_FLAG_ACTIVE : 0);
        receipt.packet.keyboard_leds = keyboard_leds;
        kms_ack_flush(&receipt, receipt_send, NULL);
        kms_ack_flush(&text_receipt, receipt_send, NULL);
        bool led_changed = keyboard_leds.sequence != last_led_sequence &&
                           kms_time_elapsed(now, last_led_status, KMS_LED_RADIO_MIN_MS);
        if (!receipt.pending && !text_receipt.pending && peers[0].record.valid && (kms_time_elapsed(now, last_status, KMS_STATUS_MS) || led_changed)) {
            kms_packet_t status = {.type = KMS_STATUS, .slot = peers[0].record.slot, .session = boot_id,
                .receiver = guard.session, .generation = guard.generation, .sequence = guard.sequence,
                .flags = (ready ? KMS_FLAG_USB_READY : 0) | (guard.active ? KMS_FLAG_ACTIVE : 0),
                .keyboard_leds = keyboard_leds};
            status.flags |= KMS_FLAG_JIGGLER_SUPPORTED | KMS_FLAG_JIGGLER_DISTANCE | KMS_FLAG_JIGGLER_MODES |
                (jiggler.enabled ? KMS_FLAG_JIGGLER_ENABLED : 0) | (jiggler.mode << KMS_JIGGLER_MODE_SHIFT);
            status.motion[0] = jiggler.interval_seconds ? jiggler.interval_seconds : KMS_JIGGLER_DEFAULT_SECONDS;
            status.motion[1] = jiggler.session; status.motion[2] = jiggler.sequence;
            status.motion[3] = jiggler.distance ? jiggler.distance : 1;
            kms_batch_advertise(&status);
            if (usb_suspended) status.public_key[6] = KMS_STATUS_USB_SUSPENDED;
            if (send_packet(peers[0].record.mac, &status)) {
                last_status = last_led_status = now;
                last_led_sequence = keyboard_leds.sequence;
            }
        }
        management_tick(usb_ready);
        portENTER_CRITICAL(&tx_lock); radio_busy = tx_busy; portEXIT_CRITICAL(&tx_lock);
        if (!radio_busy && !receipt.pending && !text_receipt.pending) kms_radio_channel_probe_reply_tick(pairing.open);
        return;
    }
    // An explicit boot preference waits for that dongle; manual selection ends startup selection.
    if (!initial_selected && !sender.slot && !pairing.open) {
        for (unsigned s = 1; s <= KMS_SLOTS; ++s) if ((!boot_slot || s == boot_slot) && peers[s - 1].boot && (peers[s - 1].flags & KMS_FLAG_USB_READY)) {
            if (kms_link_select(NULL, s)) fault = true;
            break;
        }
    }
    if (text_tx.pending && (text_tx.packet.slot != sender.slot || text_tx.packet.receiver != sender.receiver ||
        text_tx.packet.generation != sender.generation || !kms_link_typing_available()))
        memset(&text_tx, 0, sizeof(text_tx));
    if (text_tx.pending) kms_text_tx_tick(&text_tx, kms_now(), delivery_send, NULL);
    else kms_delivery_tick(&sender, kms_now());
    if (sender.block) { fault = true; sender.block = false; }
    kms_diagnostics_tick();
    management_tick(usb_ready);
    jiggler_tick(now);
}

#if CONFIG_IDF_TARGET_ESP32S3
static bool channel_probe_idle(bool busy)
{
    return !busy && !sender.count && !sender.has_pending && !sender.releasing &&
           !sender.need_activate && !text_tx.pending;
}
void kms_link_controller_recovery_tick(void)
{
    uint32_t now = kms_now();
    channel_restore_route(now);
    bool receiver_checked = false;
    if (kms_channel_recovery_monitoring()) {
        for (unsigned i = 0; i < KMS_SLOTS; ++i) {
            const peer_t *p = &peers[i];
            uint32_t boot = p->record.valid ? p->boot : 0;
            kms_channel_recovery_peer(i + 1, boot, p->seen, boot && kms_remote_radio_probe_supported(i + 1, boot));
        }
        // A lone peer's eligible restart gets first use of the recovery slot.
        receiver_checked = kms_channel_recovery_single_reachable(now);
        if (receiver_checked) receiver_recovery_tick(now);
        portENTER_CRITICAL(&tx_lock);
        bool busy = tx_busy, blocked = radio_tx.blocked;
        bool rejected = radio_rejection_seen && now - radio_rejected_at < KMS_RECOVERY_QUIET_MS;
        portEXIT_CRITICAL(&tx_lock);
        bool observe = !pairing.open && !kms_test_mode_active() && !kms_remote_transfer_busy() &&
            !kms_link_receiver_restart_in_progress() && (!recovery_global_wait || now - recovery_global_at >= 30000u) &&
            !rejected && (!processing_fault_seen || now - processing_fault_at >= 1500u);
        kms_channel_recovery_tick(now, sender.slot, observe, channel_probe_idle(busy), blocked);
    }
    if (!receiver_checked) receiver_recovery_tick(now);
}
#endif

void kms_link_status(void)
{
    radio_trace("status");
    kms_radio_channel_status(0, "OK");
    ESP_LOGI(TAG, "Selected %u, pairing %s, buffered %u, pending %s, recovering %s",
        master ? sender.slot : 0, pairing.open ? "yes" : "no", master ? sender.count : 0,
        master && sender.has_pending ? "yes" : "no", master && sender.recovering ? "yes" : "no");
    ESP_LOGI(TAG, "Automatic handover update guard: %s", kms_remote_transfer_busy() ? "blocked" : "clear");
    if (master) for (unsigned i = 0; i < KMS_SLOTS; ++i) if (peers[i].record.valid) {
        const kms_receiver_recovery_t *p = &receiver_recovery[i];
        uint32_t wait = p->cooldown && (int32_t)(p->next_at - kms_now()) > 0 ? p->next_at - kms_now() : 0;
        ESP_LOGI(TAG, "KMSRECOVERY status slot=%u enabled=%u faults=%" PRIu32 " reasons=0x%" PRIx32
            " waiting=%u attempts=%u level=%u cooldown_ms=%" PRIu32, i + 1, kms_remote_recovery_enabled(),
            p->faults, p->reasons, p->waiting, p->attempts, p->level, wait);
    }
    for (unsigned i = 0; i < KMS_SLOTS; ++i) if (peers[i].record.valid)
        ESP_LOGI(TAG, "Slot %u: " MACSTR " USB=%s last seen=%" PRIu32 "ms RSSI=%ddBm", peers[i].record.slot,
                 MAC2STR(peers[i].record.mac), peers[i].flags & KMS_FLAG_USB_READY ? "ready" : "offline",
                 kms_now() - peers[i].seen, peers[i].rssi);
    if (master) {
        for (unsigned i = 0; i < KMS_SLOTS; ++i) if (jiggler_peers[i].known)
            ESP_LOGI(TAG, "Mouse jiggler dongle %u: %s, interval %u seconds, distance %u counts, mode %s%s", i + 1,
                jiggler_peers[i].enabled ? "on" : "off", jiggler_peers[i].interval_seconds,
                jiggler_peers[i].distance, kms_jiggler_mode_name(jiggler_peers[i].mode),
                jiggler_peers[i].pending ? " (awaiting confirmation)" : "");
        uint8_t leds; bool known = kms_link_keyboard_leds(&leds);
        ESP_LOGI(TAG, "Selected keyboard LEDs: %s mask=0x%02x", known ? "known" : "unknown", leds);
    }
}
bool kms_link_typing_available(void)
{
    return master && !text_tx.failed && sender.slot && sender.have_status && (sender.flags & KMS_FLAG_USB_READY) && !sender.suppressed && !sender.block &&
           !sender.recovering && !sender.draining && !sender.need_activate && !sender.releasing &&
           !pairing.open && !kms_remote_master_updating() &&
           !kms_radio_channel_changing() &&
           !kms_time_elapsed(kms_now(), sender.seen, KMS_PEER_TIMEOUT_MS);
}
bool kms_link_typing_ready(void)
{ return kms_link_typing_available() && !sender.has_pending && !sender.count && !text_tx.pending; }
static void start_text(unsigned operation, const char *text, size_t length)
{
    if (!master) return;
    if (!++text_sequence) ++text_sequence;
    kms_packet_t p = {.slot = (uint8_t)sender.slot, .session = boot_id, .receiver = sender.receiver,
        .generation = sender.generation, .sequence = text_sequence};
    if (kms_text_packet(&p, operation, text, length)) kms_text_tx_start(&text_tx, &p, kms_now());
}
void kms_link_text_begin(void)
{
    memset(&text_tx, 0, sizeof(text_tx));
    start_text(KMS_TEXT_QUERY, NULL, 0);
}
bool kms_link_text_supported(void) { return text_tx.supported; }
bool kms_link_text_send(const char *text, size_t length)
{
    if (!kms_link_typing_ready() || !text_tx.supported || !length || length > KMS_TEXT_BYTES) return false;
    start_text(KMS_TEXT_DATA, text, length); return true;
}
void kms_link_text_cancel(void)
{
    if (text_tx.supported) start_text(KMS_TEXT_CANCEL, NULL, 0);
}
bool kms_link_text_active(void) { return text_rx.active; }
bool kms_link_text_next(kms_sample_t *out)
{
    if (!text_rx.active || !guard.active || !keyboard_leds.valid) return false;
    bool ready_report = kms_text_rx_next(&text_rx, (keyboard_leds.leds & KMS_LOCK_CAPS) != 0, out);
    if (text_rx.complete && text_rx.packet.flags == KMS_TEXT_DATA) {
        kms_text_ack(&text_rx.packet, &text_receipt.packet); text_receipt.pending = true;
    }
    return ready_report;
}
void kms_link_text_delivered(void)
{
    kms_text_rx_delivered(&text_rx);
    if (text_rx.complete && text_rx.packet.flags == KMS_TEXT_DATA) {
        kms_text_ack(&text_rx.packet, &text_receipt.packet); text_receipt.pending = true;
    }
}
unsigned kms_link_selected(void) { return master ? sender.slot : 0; }
bool kms_link_pairing(void) { return pairing.open; }
bool kms_link_take_fault(void) { bool f = fault; fault = false; return f; }
bool kms_link_error(void)
{
    if (error_active && (int32_t)(error_until - kms_now()) <= 0) error_active = false;
    return error_active;
}
void kms_link_usb_fault(void)
{
    if (!master) kms_radio_channel_processing_fault(kms_now());
    fail_link(KMS_EVENT_USB_STALL);
}
void kms_link_input_overflow(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    processing_fault_at = kms_now();
    processing_fault_seen = true;
#endif
    if (master && sender.slot) kms_delivery_abandon(&sender, KMS_EVENT_INPUT_OVERFLOW, kms_now());
    else kms_diagnostics_event(KMS_EVENT_INPUT_OVERFLOW, 0, 0);
}
void kms_link_set_keyboard_leds(const kms_led_state_t *s) { if (!master) keyboard_leds = *s; }
bool kms_link_keyboard_leds(uint8_t *leds)
{
    bool known = master && sender.slot && !sender.recovering && !pairing.open && led_feedback.received && led_feedback.state.valid;
    *leds = known ? led_feedback.state.leds : 0;
    return known;
}
