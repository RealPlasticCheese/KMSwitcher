// Production transmitter and scheduler; SDK lifecycle calls and clock are fake.
#include "kms_core.h"
#include "kms_radio.h"
#include "kms_management.h"
#include "kms_stream.h"
#include "kms_delivery.h"
#include "kms_ack.h"
#include "kms_slave_history.h"
#include "kms_receiver_recovery.h"
#include "kms_text.h"
#include <inttypes.h>
#include <string.h>
#define CONFIG_IDF_TARGET_ESP32S3 1

typedef int esp_err_t;
typedef struct { unsigned unused; } esp_now_send_info_t;
typedef struct { unsigned unused; } esp_now_recv_info_t;
typedef unsigned esp_now_send_status_t;
typedef struct { unsigned channel, ifidx; bool encrypt; uint8_t peer_addr[6], lmk[16]; } esp_now_peer_info_t;
enum { ESP_OK, ESP_FAIL, ESP_NOW_SEND_SUCCESS = 0, WIFI_IF_STA = 0, WIFI_PS_NONE = 0, WIFI_SECOND_CHAN_NONE = 0,
       KMS_LINK_KEY_BYTES = 16 };
static bool master = true, tx_busy, transferring, confirmed = true, changing, trial, testing;
static unsigned probe_completions, driver_stalls;
static uint32_t last_probe_token, last_probe_start, last_probe_elapsed;
static bool last_probe_failed;
static void kms_channel_recovery_mac(uint32_t token, uint32_t started, uint32_t elapsed, bool failed)
{
    ++probe_completions;
    last_probe_token = token; last_probe_start = started; last_probe_elapsed = elapsed; last_probe_failed = failed;
}
static void kms_channel_recovery_driver_stall(void) { if (trial) ++driver_stalls; }
static struct { bool open; } pairing;
static int tx_lock;
static uint32_t clock_ms, clipboard_deferred;
static uint32_t radio_tx_last_ms, radio_tx_slow, radio_rejected_at;
static bool radio_rejection_seen;
static uint32_t recovery_errors[KMS_SLOTS], recovery_reasons[KMS_SLOTS];
static unsigned captured_fault, fault_count;
static void kms_slave_diagnostics_fault(unsigned reason, unsigned op, uint32_t duration, uint32_t failed)
{ (void)op; (void)duration; (void)failed; captured_fault = reason; ++fault_count; }
static unsigned checks, wakeups, sdk_calls, stopped, started, rebuilt, sends, fail_call;
static bool driver_running = true;
static esp_err_t send_result;
static const uint8_t broadcast[6] = {255,255,255,255,255,255};
static const uint8_t peer_mac[6] = {2,0,0,0,0,2};
#include "link_select_types.inc"
static peer_t peers[KMS_SLOTS];
static kms_delivery_t sender;
static kms_text_tx_t text_tx;
static kms_ack_t receipt, text_receipt;
#include "link_transmit_types.inc"
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static void fake_log(const char *format, ...) { (void)format; }
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "fake error"; }
#define ESP_LOGI(tag, ...) fake_log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log(__VA_ARGS__)
#define ESP_LOGE(tag, ...) fake_log(__VA_ARGS__)
#define ESP_LOGD(tag, ...) fake_log(__VA_ARGS__)
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(mac) (mac)[0], (mac)[1], (mac)[2], (mac)[3], (mac)[4], (mac)[5]
#define ESP_ERROR_CHECK(e) ((void)(e))
static uint32_t kms_now(void) { return clock_ms; }
static bool same(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }
static void kms_app_wake(void) { ++wakeups; }
static bool kms_remote_transfer_busy(void) { return transferring; }
static bool kms_ota_confirmed(void) { return confirmed; }
static bool kms_radio_channel_changing(void) { return changing; }
static bool kms_radio_channel_trial_active(void) { return trial; }
static bool kms_test_mode_active(void) { return testing; }
static unsigned kms_radio_channel_get(void) { return 11; }
static void kms_slave_diagnostics_add(unsigned kind, unsigned n) { (void)kind; (void)n; }
static void kms_slave_diagnostics_event(unsigned kind, uint32_t detail) { (void)kind; (void)detail; }
static esp_err_t sdk_result(void) { return ++sdk_calls == fail_call ? ESP_FAIL : ESP_OK; }
static bool esp_now_is_peer_exist(const uint8_t *mac) { (void)mac; return false; }
static esp_err_t esp_now_mod_peer(const esp_now_peer_info_t *p) { (void)p; return sdk_result(); }
static esp_err_t esp_now_add_peer(const esp_now_peer_info_t *p)
{
    ++rebuilt;
    if (rebuilt == 1 && (p->encrypt || memcmp(p->peer_addr, broadcast, 6))) return ESP_FAIL;
    if (rebuilt == 2 && (!p->encrypt || memcmp(p->peer_addr, peer_mac, 6) || p->lmk[0] != 42)) return ESP_FAIL;
    return sdk_result();
}
static esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *wire, size_t n)
{ (void)mac; (void)wire; (void)n; ++sends; return driver_running ? send_result : ESP_FAIL; }
static esp_err_t esp_now_deinit(void) { return sdk_result(); }
static esp_err_t esp_wifi_stop(void) { ++stopped; driver_running = false; return sdk_result(); }
static esp_err_t esp_wifi_start(void) { ++started; driver_running = true; return sdk_result(); }
static esp_err_t esp_wifi_set_ps(unsigned mode) { (void)mode; return sdk_result(); }
static esp_err_t esp_wifi_set_channel(unsigned channel, unsigned secondary)
{ (void)secondary; return channel == 11 ? sdk_result() : ESP_FAIL; }
static esp_err_t esp_now_init(void) { return sdk_result(); }
static esp_err_t esp_now_register_recv_cb(void (*callback)(const esp_now_recv_info_t *, const uint8_t *, int))
{ (void)callback; return sdk_result(); }
static esp_err_t esp_now_register_send_cb(void (*callback)(const esp_now_send_info_t *, esp_now_send_status_t))
{ (void)callback; return sdk_result(); }
static void kms_stream_radio_rate(const uint8_t *mac, const uint8_t *wire, size_t size)
{ (void)mac; (void)wire; (void)size; }
static void kms_stream_radio_complete(bool success, uint32_t elapsed) { (void)success; (void)elapsed; }
#include "link_transmit.inc"
static uint32_t radio_rx_last_ms, radio_rx_max_ms, radio_rx_frames, radio_rx_at, radio_rx_gap, radio_tick_max;
static uint32_t boot_id = 0x42;
static bool ready = true;
static unsigned incoming = 1;
static int rx_lock;
static unsigned uxQueueMessagesWaiting(unsigned queue) { (void)queue; return 3; }
static uint32_t esp_get_minimum_free_heap_size(void) { return 30000; }
#include "link_radio_status.inc"
static void receive(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{ (void)info; (void)data; (void)len; }
static unsigned desktop_queue, remote_queue;
static bool take(unsigned *queued, kms_management_frame_t *frame, unsigned op)
{ if (!*queued) return false; --*queued; frame->op = (uint8_t)op; return true; }
static bool kms_desktop_take_outgoing(kms_management_frame_t *f) { return take(&desktop_queue, f, KMS_M_DESKTOP); }
static bool kms_remote_take_outgoing(kms_management_frame_t *f) { return take(&remote_queue, f, KMS_M_STATS); }
#include "link_management.inc"
#include "link_channel_idle.inc"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static void reset(void)
{
    memset(&radio_tx, 0, sizeof(radio_tx)); memset(&sender, 0, sizeof(sender));
    memset(radio_peers, 0, sizeof(radio_peers)); memset(radio_slow, 0, sizeof(radio_slow)); radio_slow_next = 0;
    memset(&text_tx, 0, sizeof(text_tx)); memset(&receipt, 0, sizeof(receipt)); memset(&text_receipt, 0, sizeof(text_receipt));
    memset(peers, 0, sizeof(peers)); peers[0].record.valid = peers[0].record.slot = 1;
    memcpy(peers[0].record.mac, peer_mac, 6); peers[0].record.key[0] = 42;
    clock_ms = 100; master = confirmed = driver_running = true;
    tx_busy = transferring = changing = trial = testing = pairing.open = false;
    probe_completions = driver_stalls = 0;
    wakeups = sdk_calls = stopped = started = rebuilt = sends = fail_call = 0;
    desktop_queue = remote_queue = clipboard_deferred = 0; send_result = ESP_OK;
}
__declspec(dllexport) int kms_run_link_transmit_tests(void)
{
    checks = 0; reset();
    CHECK(send_frame(peer_mac, NULL, 0) && tx_busy && radio_tx.submitted == 1);
    CHECK(!send_frame(peer_mac, NULL, 0) && sends == 1);
    clock_ms += 37; sent(NULL, ESP_NOW_SEND_SUCCESS);
    CHECK(!tx_busy && radio_tx.completed == 1 && !radio_tx.failed && radio_tx.max_ms == 37 && wakeups == 1);
    CHECK(send_frame(peer_mac, NULL, 0)); clock_ms += 23; sent(NULL, 1);
    CHECK(radio_tx.failed == 1 && radio_tx.completed == 2 && !tx_busy);
    send_result = ESP_FAIL; CHECK(!send_frame(peer_mac, NULL, 0));
    CHECK(!tx_busy && radio_tx.rejected == 1 && radio_tx.last_error == ESP_FAIL);
    sent(NULL, 0); CHECK(radio_tx.unexpected == 1);
    // Per-peer evidence separates input failures from desktop/clipboard airtime.
    reset(); uint8_t wire[KMS_M_MAX_WIRE];
    kms_packet_t input = {.type = KMS_INPUT, .slot = 1, .sequence = 41};
    size_t length = kms_wire_encode(&input, wire, sizeof(wire));
    CHECK(send_frame(peer_mac, wire, length)); clock_ms += 120; sent(NULL, 1);
    CHECK(radio_peers[0][RADIO_INPUT].completed == 1 && radio_peers[0][RADIO_INPUT].failed == 1);
    CHECK(radio_peers[0][RADIO_INPUT].max_ms == 120 && radio_slow_next == 1);
    CHECK(radio_slow[0].slot == 1 && radio_slow[0].op == KMS_INPUT && radio_slow[0].sequence == 41);
    kms_management_frame_t desktop = {.op = KMS_M_DESKTOP, .slot = 1, .id = 42, .length = 64};
    desktop.data[2] = 10;
    length = kms_management_encode(&desktop, wire, sizeof(wire));
    CHECK(send_frame(peer_mac, wire, length)); clock_ms += 90; sent(NULL, 0);
    CHECK(radio_peers[0][RADIO_DESKTOP].completed == 1 && !radio_peers[0][RADIO_DESKTOP].failed);
    CHECK(radio_slow[1].kind == RADIO_DESKTOP && radio_slow[1].op == 10 && radio_slow[1].sequence == 42);
    // No completion: keep the gate closed until the old driver is stopped.
    reset(); sender.generation = 19; sender.pending.sequence = 23; sender.has_pending = true;
    CHECK(send_frame(peer_mac, NULL, 0)); clock_ms += KMS_RADIO_TX_TIMEOUT_MS - 1;
    radio_health_tick(); CHECK(!stopped && tx_busy);
    ++clock_ms; radio_health_tick();
    CHECK(stopped == 1 && started == 1 && rebuilt == 2 && !tx_busy && !radio_tx.blocked && radio_tx.recoveries == 1);
    CHECK(sender.generation == 19 && sender.pending.sequence == 23 && sender.has_pending);
    CHECK(send_frame(peer_mac, NULL, 0)); sent(NULL, 0);
    // Every disruptive state defers the watchdog, even when the deadline passes.
    for (unsigned state = 0; state < 6; ++state) {
        reset(); CHECK(send_frame(peer_mac, NULL, 0)); clock_ms += KMS_RADIO_TX_TIMEOUT_MS;
        switch (state) {
        case 0: master = false; break; case 1: transferring = true; break;
        case 2: confirmed = false; break; case 3: changing = true; break;
        case 4: testing = true; break; default: pairing.open = true;
        }
        radio_health_tick(); CHECK(!stopped && tx_busy && !radio_tx.blocked);
        master = confirmed = true; transferring = changing = testing = pairing.open = false;
        sent(NULL, 0); radio_health_tick(); CHECK(!stopped);
    }
    // Recovery never loops on SDK errors or bypasses its finite attempt budget.
    for (unsigned call = 1; call <= 10; ++call) {
        reset(); fail_call = call; CHECK(send_frame(peer_mac, NULL, 0));
        clock_ms += KMS_RADIO_TX_TIMEOUT_MS; radio_health_tick();
        CHECK(radio_tx.blocked && radio_tx.last_error == ESP_FAIL && !radio_tx.recoveries);
        unsigned before = sdk_calls; sent(NULL, 0); radio_health_tick();
        CHECK(!send_frame(peer_mac, NULL, 0) && sdk_calls == before);
    }
    reset();
    for (unsigned attempt = 0; attempt < KMS_RADIO_RECOVERY_ATTEMPTS; ++attempt) {
        rebuilt = 0; CHECK(send_frame(peer_mac, NULL, 0));
        clock_ms += KMS_RADIO_TX_TIMEOUT_MS; radio_health_tick();
        CHECK(!radio_tx.blocked && radio_tx.recoveries == attempt + 1);
    }
    CHECK(send_frame(peer_mac, NULL, 0)); clock_ms += KMS_RADIO_TX_TIMEOUT_MS;
    radio_health_tick(); CHECK(radio_tx.blocked && stopped == KMS_RADIO_RECOVERY_ATTEMPTS);
    // A new quiet window replenishes the budget before another stall; wrap-safe.
    reset(); clock_ms = 0xfffffff0u; CHECK(send_frame(peer_mac, NULL, 0));
    clock_ms += KMS_RADIO_TX_TIMEOUT_MS; radio_health_tick(); CHECK(radio_tx.recoveries == 1);
    clock_ms += KMS_RADIO_RECOVERY_WINDOW_MS; radio_tx.attempts = KMS_RADIO_RECOVERY_ATTEMPTS;
    rebuilt = 0; CHECK(send_frame(peer_mac, NULL, 0));
    clock_ms += KMS_RADIO_TX_TIMEOUT_MS; radio_health_tick();
    CHECK(!radio_tx.blocked && radio_tx.attempts == 1 && radio_tx.recoveries == 2);
    // A free MAC transmit slot cannot let bulk data jump an in-flight input ACK.
    reset(); kms_management_frame_t frame; sender.has_pending = true;
    CHECK(!bulk_allowed() && clipboard_deferred == 1);
    desktop_queue = remote_queue = 1;
    CHECK(take_management_frame(&frame) && frame.op == KMS_M_DESKTOP);
    CHECK(take_management_frame(&frame) && frame.op == KMS_M_STATS);
    sender.has_pending = false; sender.recovering = true; CHECK(!bulk_allowed());
    sender.recovering = false; sender.need_activate = true; CHECK(!bulk_allowed());
    sender.need_activate = false; sender.releasing = true; CHECK(!bulk_allowed());
    sender.releasing = false; text_tx.pending = true; CHECK(!bulk_allowed());
    text_tx.pending = false; CHECK(bulk_allowed());
    CHECK(channel_probe_idle(false) && !channel_probe_idle(true));
    sender.count = 1; CHECK(!channel_probe_idle(false)); sender.count = 0;
    sender.has_pending = true; CHECK(!channel_probe_idle(false)); sender.has_pending = false;
    sender.releasing = true; CHECK(!channel_probe_idle(false)); sender.releasing = false;
    sender.need_activate = true; CHECK(!channel_probe_idle(false)); sender.need_activate = false;
    text_tx.pending = true; CHECK(!channel_probe_idle(false)); text_tx.pending = false;
    reset();
    uint8_t probe_wire[KMS_RADIO_BYTES] = {'K','M','S','C',1,KMS_RADIO_PROBE};
    kms_put_u32(probe_wire + 12, 4242);
    uint32_t previous_errors = recovery_errors[0];
    CHECK(send_frame(peer_mac, probe_wire, sizeof(probe_wire)));
    clock_ms += 120;
    sent(NULL, 1);
    CHECK(probe_completions == 1 && last_probe_token == 4242 && last_probe_start == 100 &&
          last_probe_elapsed == 120 && last_probe_failed && recovery_errors[0] == previous_errors);
    reset(); trial = changing = true;
    CHECK(send_frame(peer_mac, probe_wire, sizeof(probe_wire)));
    clock_ms += KMS_RADIO_TX_TIMEOUT_MS;
    radio_health_tick();
    CHECK(driver_stalls == 1 && stopped == 1 && !tx_busy && !radio_tx.blocked);
    master = false; receipt.pending = true; CHECK(!bulk_allowed());
    receipt.pending = false; text_receipt.pending = true; CHECK(!bulk_allowed());
    text_receipt.pending = false; CHECK(bulk_allowed());
    reset(); master = false; clock_ms = 6000; fault_count = 0;
    CHECK(send_frame(peer_mac, NULL, 0)); clock_ms += 123; sent(NULL, 1);
    CHECK(fault_count == 1 && captured_fault == KMS_RF_MAC_FAILED);
    CHECK(send_frame(peer_mac, NULL, 0)); clock_ms += 140; sent(NULL, 0);
    CHECK(fault_count == 2 && captured_fault == KMS_RF_TX_SLOW);
    radio_receive_timing(clock_ms - 75);
    CHECK(captured_fault == KMS_RF_DISPATCH);
    uint8_t diagnostic[KMS_RADIO_STATUS_WORDS * 4];
    CHECK(!kms_link_radio_status(diagnostic, sizeof(diagnostic) - 1));
    CHECK(kms_link_radio_status(diagnostic, sizeof(diagnostic)) == sizeof(diagnostic));
    CHECK(kms_get_u32(diagnostic) == 1 && kms_get_u32(diagnostic + 4) == boot_id);
    CHECK(kms_get_u32(diagnostic + 20) == radio_tx.failed && kms_get_u32(diagnostic + 56) >= 75);
    CHECK(kms_get_u32(diagnostic + 68) == 3 && kms_get_u32(diagnostic + 76) == 30000);
    return 0;
}
__declspec(dllexport) unsigned kms_link_transmit_test_count(void) { return checks; }
