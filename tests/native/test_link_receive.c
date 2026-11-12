// The runner includes the production receive callback and queue item types.
// ESP-NOW delivery and FreeRTOS queues are adapters; codecs are production code.
#include "kms_core.h"
#include "kms_management.h"
#include "kms_radio.h"
#include "kms_delivery.h"
#include "kms_receiver_recovery.h"
#include <string.h>
#include <inttypes.h>
#ifdef TEST_RECEIVER_INPUT
#define CONFIG_IDF_TARGET_ESP32S3 0
#else
#define CONFIG_IDF_TARGET_ESP32S3 1
#endif

typedef struct { int8_t rssi; } wifi_pkt_rx_ctrl_t;
typedef struct {
    const uint8_t *src_addr, *des_addr;
    const wifi_pkt_rx_ctrl_t *rx_ctrl;
} esp_now_recv_info_t;
enum { ESP_NOW_ETH_ALEN = 6, pdTRUE = 1 };
static const uint8_t own_mac[6] = {2, 0, 0, 0, 0, 1};
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
static const uint8_t paired_mac[6] = {2, 0, 0, 0, 0, 2};
static const uint8_t other_mac[6] = {2, 0, 0, 0, 0, 3};
static unsigned incoming = 1, management_incoming = 2, channel_incoming = 3;
static unsigned counts[4], checks;
static uint8_t captured[KMS_M_MAX_WIRE + sizeof(kms_packet_t)];
static bool rx_overflow;
static int rx_lock;
static unsigned wakeups;
static void kms_app_wake(void) { ++wakeups; }
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define kms_test_record(...) ((void)0)
static bool same(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }
static int xQueueSend(unsigned queue, const void *item, unsigned wait);
static uint32_t kms_now(void);
static void kms_stream_radio_receive(const uint8_t *wire, size_t size, const uint8_t *mac, int8_t rssi)
{ (void)wire; (void)size; (void)mac; (void)rssi; }
#include "link_receive.inc"
// The remaining fixtures exercise the controller's routing state machine.
#undef CONFIG_IDF_TARGET_ESP32S3
#define CONFIG_IDF_TARGET_ESP32S3 1
static int xQueueSend(unsigned queue, const void *item, unsigned wait)
{
    (void)wait;
    ++counts[queue];
    size_t bytes = queue == incoming ? sizeof(queued_input_t) :
        queue == management_incoming ? sizeof(management_received_t) : sizeof(channel_received_t);
    memcpy(captured, item, bytes);
    return pdTRUE;
}
// Exercise the production selection gate with real delivery state transitions.
static bool master = true, recovery_warning_started, initial_selected, fault;
static struct { bool open; } pairing;
static kms_delivery_t sender;
static kms_buffered_t replay[KMS_RECOVERY_BUFFER_DEPTH];
static kms_led_feedback_t led_feedback;
static uint32_t clock_ms = 100, last_selection, route_epoch, boot_id = 99;
static bool updating, channel_changing, channel_trial, channel_grace, manual_restart_pending;
static bool usb_suspended, receiver_host_available;
static bool processing_fault_seen;
static uint32_t processing_fault_at;
static unsigned channel_cancellations, recovery_owner, paused_slot;
static struct { unsigned slot; uint32_t epoch, after; bool waiting, manual; } channel_route;
static bool kms_remote_transfer_busy(void);
static bool kms_test_mode_active(void);
static bool kms_remote_master_updating(void) { return updating; }
static bool kms_remote_master_restart_pending(void) { return manual_restart_pending; }
static bool kms_radio_channel_trial_active(void) { return channel_trial; }
static bool kms_radio_channel_changing(void) { return channel_changing || channel_trial; }
static bool kms_radio_channel_recovering(void) { return kms_radio_channel_changing() || channel_grace; }
static void kms_channel_recovery_cancel(void) { if (channel_trial) ++channel_cancellations; }
static uint32_t kms_now(void) { return clock_ms; }
static void start_error(uint32_t now) { (void)now; }
static void fake_log(const char *format, ...) { (void)format; }
#define ESP_LOGW(tag, ...) fake_log(__VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_log(__VA_ARGS__)
#define kms_diagnostics_event(...) ((void)0)
#define printf(...) ((void)0)
// Peer types precede the function so the production declaration is usable here.
enum { KMS_LINK_KEY_BYTES = 16 };
#include "link_select_types.inc"
static peer_t peers[KMS_SLOTS];
#include "link_select.inc"
static unsigned saves, removed_slot;
static unsigned jiggler_peers[KMS_SLOTS];
#include "link_pause.inc"
#include "link_resume.inc"
static void esp_now_del_peer(const uint8_t *mac) { (void)mac; }
static void kms_radio_channel_forget(unsigned slot) { removed_slot = slot; }
static void kms_remote_peer(unsigned slot, bool paired, uint32_t boot, uint32_t seen, uint8_t flags, const uint8_t *mac)
{ (void)slot; (void)paired; (void)boot; (void)seen; (void)flags; (void)mac; }
static void save(void) { ++saves; }
#include "link_unpair.inc"
static bool desktop_idle;
static void kms_desktop_motion(int32_t x, int32_t y) { (void)x; (void)y; }
static void kms_diagnostics_buffer(unsigned count) { (void)count; }
#include "link_emit.inc"
#include "link_route_starting.inc"
static unsigned recovery_resume_slot;
static uint32_t recovery_resume_epoch, recovery_global_at;
static bool recovery_global_wait, recovery_initialized[KMS_SLOTS];
static bool recovery_online[KMS_SLOTS];
static uint8_t recovery_macs[KMS_SLOTS][6];
static kms_receiver_recovery_t receiver_recovery[KMS_SLOTS];
static uint32_t recovery_seen_errors[KMS_SLOTS], recovery_errors[KMS_SLOTS], recovery_reasons[KMS_SLOTS];
static struct { bool blocked; } radio_tx;
static int tx_lock;
static uint32_t radio_rejected_at;
static bool radio_rejection_seen, transfer_busy, test_mode, auto_enabled = true, auto_supported = true, auto_confirmed = true;
static bool auto_queue = true, result_pending;
static unsigned restart_requests, requested_slot;
static bool requested_active;
static kms_recovery_budget_t saved_budget, requested_budget;
typedef struct { unsigned slot, status; uint32_t boot; } kms_recovery_result_t;
static kms_recovery_result_t auto_result;
static bool kms_remote_transfer_busy(void) { return transfer_busy || updating; }
static bool kms_test_mode_active(void) { return test_mode; }
static bool kms_remote_recovery_enabled(void) { return auto_enabled; }
static bool kms_remote_recovery_supported(unsigned slot, uint32_t boot, bool *confirmed)
{ (void)slot; (void)boot; *confirmed = auto_confirmed; return auto_supported; }
static void kms_remote_recovery_budget(unsigned slot, const uint8_t mac[6], kms_recovery_budget_t *out)
{ (void)slot; *out = saved_budget; memcpy(out->mac, mac, 6); }
static bool kms_remote_recovery_request(unsigned slot, uint32_t boot, const kms_recovery_budget_t *budget, bool active)
{ (void)boot; ++restart_requests; requested_slot = slot; requested_active = active; requested_budget = *budget; return auto_queue; }
static bool kms_remote_take_recovery_result(kms_recovery_result_t *out)
{ if (!result_pending) return false; result_pending = false; *out = auto_result; return true; }
#include "link_recovery.inc"
static kms_packet_t submitted;
static unsigned submissions;
static bool submit(void *ctx, const kms_packet_t *packet)
{ (void)ctx; submitted = *packet; ++submissions; return true; }
static void acknowledge(uint32_t now)
{
    kms_packet_t ack = {.type = KMS_ACK, .slot = submitted.slot, .session = submitted.session,
        .receiver = submitted.receiver, .generation = submitted.generation, .sequence = submitted.sequence,
        .flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE};
    kms_delivery_receive(&sender, &ack, now);
}
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned queued(void) { return counts[1] + counts[2] + counts[3]; }

__declspec(dllexport) int kms_run_link_receive_tests(void)
{
    checks = wakeups = 0; memset(counts, 0, sizeof(counts));
    wifi_pkt_rx_ctrl_t signal = {-42};
    // The source is deliberately the paired master's MAC in every spoof case.
    esp_now_recv_info_t info = {paired_mac, broadcast, &signal};
    kms_management_frame_t frame = {.slot = 1, .id = 123, .sender = 456, .target = 789};
    uint8_t wire[(unsigned)KMS_M_MAX_WIRE > (unsigned)KMS_MAX_WIRE_SIZE ? KMS_M_MAX_WIRE : KMS_MAX_WIRE_SIZE];
    for (unsigned op = KMS_M_IDENTITY; op <= KMS_M_RADIO_STATUS; ++op) {
        frame.op = (uint8_t)op;
        size_t bytes = kms_management_encode(&frame, wire, sizeof(wire));
        CHECK(bytes != 0);
        unsigned before = queued();
        info.des_addr = broadcast; receive(&info, wire, (int)bytes);
        CHECK(queued() == before); // No broadcast query, write, activation or abort.
        info.des_addr = other_mac; receive(&info, wire, (int)bytes);
        CHECK(queued() == before);
        info.des_addr = own_mac; receive(&info, wire, (int)bytes);
        CHECK(counts[2] == op);
        const management_received_t *message = (const void *)captured;
        CHECK(message->frame.op == op && message->frame.id == frame.id &&
            message->rssi == -42 && same(message->mac, paired_mac));
    }
    frame.op = KMS_M_OTA_WRITE; frame.length = KMS_M_DATA;
    frame.data[KMS_M_DATA - 1] = 0x6a;
    CHECK(kms_management_encode(&frame, wire, sizeof(wire)) == KMS_M_MAX_WIRE);
    info.des_addr = own_mac; receive(&info, wire, KMS_M_MAX_WIRE);
    CHECK(counts[2] == KMS_M_RADIO_STATUS + 1);
    CHECK(((const management_received_t *)(const void *)captured)->frame.data[KMS_M_DATA - 1] == 0x6a);
    for (unsigned type = KMS_STATUS; type <= KMS_TEXT_ACK; ++type) {
        kms_packet_t packet = {.type = (uint8_t)type, .slot = 1};
        CHECK(kms_wire_encode(&packet, wire, sizeof(wire)) == KMS_WIRE_SIZE);
        unsigned before = counts[1];
        info.des_addr = broadcast; receive(&info, wire, KMS_WIRE_SIZE);
        CHECK(counts[1] == before + (type == KMS_PAIR_REQUEST));
        before = counts[1];
        info.des_addr = own_mac; receive(&info, wire, KMS_WIRE_SIZE);
        CHECK(counts[1] == before + 1); // Normal input and every pairing step survive.
        const queued_input_t *message = (const void *)captured;
        CHECK(message->length == KMS_WIRE_SIZE && !memcmp(message->wire, wire, KMS_WIRE_SIZE));
        info.des_addr = other_mac; receive(&info, wire, KMS_WIRE_SIZE);
        CHECK(counts[1] == before + 1);
    }
    kms_packet_t batch = {.type = KMS_INPUT_BATCH, .slot = 1, .sequence = 1, .batch_count = 3};
    size_t batch_bytes = kms_wire_encode(&batch, wire, sizeof(wire));
    CHECK(batch_bytes == 111);
#ifdef TEST_RECEIVER_INPUT
    const unsigned accepts_batches = 1;
    CHECK(sizeof(((queued_input_t *)0)->wire) == KMS_MAX_WIRE_SIZE);
#else
    const unsigned accepts_batches = 0;
    CHECK(sizeof(((queued_input_t *)0)->wire) == KMS_WIRE_SIZE);
#endif
    unsigned batch_before = counts[1];
    info.des_addr = broadcast; receive(&info, wire, (int)batch_bytes);
    CHECK(counts[1] == batch_before);
    info.des_addr = own_mac; receive(&info, wire, (int)batch_bytes);
    CHECK(counts[1] == batch_before + accepts_batches);
    const queued_input_t *batch_message = (const void *)captured;
    kms_packet_t decoded_batch;
    _Static_assert(sizeof(queued_input_t) < sizeof(received_t),
                   "The receive queue must store compact wire packets.");
    if (accepts_batches) {
        CHECK(batch_message->length == batch_bytes && batch_message->rssi == -42 && same(batch_message->mac, paired_mac));
        CHECK(kms_wire_decode(&decoded_batch, batch_message->wire, batch_message->length));
        CHECK(decoded_batch.type == KMS_INPUT_BATCH && decoded_batch.batch_count == 3);
    }
    wire[5] = 4; receive(&info, wire, (int)batch_bytes);
    CHECK(counts[1] == batch_before + accepts_batches);
    batch.batch_count = 4;
    for (unsigned i = 0; i < 4; ++i) {
        kms_state_t *state = i ? &batch.extra[i - 1].state : &batch.state;
        for (unsigned key = 0; key < 27u + (unsigned)(i >= 2); ++key) kms_set_key(state, key, true);
    }
    batch_bytes = kms_wire_encode(&batch, wire, sizeof(wire));
    CHECK(batch_bytes == KMS_MAX_WIRE_SIZE);
    receive(&info, wire, (int)batch_bytes);
    CHECK(counts[1] == batch_before + 2 * accepts_batches);
    if (accepts_batches) {
        CHECK(batch_message->length == batch_bytes && !memcmp(batch_message->wire, wire, batch_bytes));
        CHECK(kms_wire_decode(&decoded_batch, batch_message->wire, batch_message->length) && decoded_batch.batch_count == 4);
    }
    // Channel discovery intentionally uses broadcasts and verifies its own HMAC
    // in the coordinator; receive must still pass those frames to that queue.
    memcpy(wire, "KMSC", 4);
    info.des_addr = broadcast; receive(&info, wire, KMS_RADIO_BYTES);
    CHECK(counts[3] == 1);
    info.des_addr = own_mac; receive(&info, wire, KMS_RADIO_BYTES);
    CHECK(counts[3] == 2);
    unsigned before = queued();
    receive(NULL, wire, KMS_RADIO_BYTES);
    info.src_addr = NULL; receive(&info, wire, KMS_RADIO_BYTES);
    info.src_addr = paired_mac; info.des_addr = NULL; receive(&info, wire, KMS_RADIO_BYTES);
    info.des_addr = own_mac; receive(&info, NULL, KMS_RADIO_BYTES);
    receive(&info, wire, 3);
    CHECK(queued() == before && !rx_overflow);
    CHECK(wakeups == queued()); // Rejected/spoofed traffic never wakes the owner.
    kms_delivery_init(&sender, replay, 99, NULL, NULL, NULL);
    peers[1].record.valid = true; peers[1].boot = 123; peers[1].seen = clock_ms;
    peers[1].batch_limit = 3;
    for (unsigned flags = 0; flags <= 255; ++flags) {
        peers[1].flags = (uint8_t)flags;
        CHECK(kms_link_select(NULL, 2));
        CHECK(sender.slot == 2 && sender.receiver == 123 && sender.batch_limit == 3);
        CHECK(kms_link_desktop_available(2) == ((flags & KMS_FLAG_USB_READY) != 0));
    }
    CHECK(initial_selected && !recovery_warning_started);
    CHECK(!kms_link_select(NULL, 0) && !kms_link_select(NULL, KMS_SLOTS + 1));
    CHECK(!kms_link_desktop_available(0) && !kms_link_desktop_available(KMS_SLOTS + 1));
    CHECK(!kms_link_select(NULL, 1) && sender.slot == 2);
    CHECK(!kms_link_desktop_available(1));
    clock_ms += KMS_PEER_TIMEOUT_MS;
    CHECK(!kms_link_select(NULL, 2) && sender.slot == 2);
    CHECK(!kms_link_desktop_available(2));
    peers[1].seen = clock_ms; peers[1].boot = 0;
    CHECK(!kms_link_select(NULL, 2)); CHECK(!kms_link_desktop_available(2)); peers[1].boot = 123;
    pairing.open = true; CHECK(!kms_link_select(NULL, 2)); pairing.open = false;
    updating = true; CHECK(!kms_link_select(NULL, 2)); updating = false;
    channel_changing = true; CHECK(!kms_link_select(NULL, 2)); channel_changing = false;
    CHECK(kms_link_select(NULL, 2));
    CHECK(!memcmp(kms_link_unpair(0), "INVALID", 8));
    pairing.open = true; CHECK(!memcmp(kms_link_unpair(2), "BUSY", 5)); pairing.open = false;
    CHECK(saves == 0 && peers[1].record.valid);
    peers[1].boot = 0; peers[1].seen = 0; // No live receiver required.
    jiggler_peers[1] = 42;
    CHECK(kms_link_unpair(2) == NULL);
    CHECK(!peers[1].record.valid && !peers[1].boot && !jiggler_peers[1]);
    CHECK(sender.slot == 0 && !sender.releasing && paused_slot == 0);
    CHECK(saves == 1 && removed_slot == 2);
    CHECK(!memcmp(kms_link_unpair(2), "NOT_PAIRED", 11) && saves == 1);
    peers[2].record.valid = true; sender.slot = 1; paused_slot = 3;
    CHECK(kms_link_unpair(3) == NULL && sender.slot == 1 && paused_slot == 0);
    CHECK(saves == 2);
    // Input routed after the link tick gets another iteration immediately.
    kms_delivery_init(&sender, replay, 99, NULL, NULL, NULL);
    kms_delivery_select(&sender, 1, 123, clock_ms);
    kms_state_t state = {0}; kms_set_key(&state, 4, true);
    unsigned wakes_before = wakeups;
    kms_link_emit(NULL, &state, 1, 0, 0, 0);
    CHECK(sender.count == 1 && wakeups == wakes_before + 1 && !desktop_idle);
    kms_delivery_stop(&sender); state = (kms_state_t){0};
    kms_link_emit(NULL, &state, 0, 0, 0, 0);
    CHECK(!sender.count && wakeups == wakes_before + 1 && desktop_idle);
    // Receiver-side calls cannot create or mutate controller delivery state.
    master = false;
    static kms_delivery_t receiver_before;
    receiver_before = sender;
    kms_link_emit(NULL, &state, 1, 0, 0, 0);
    CHECK(!kms_link_select(NULL, 1));
    CHECK(!memcmp(&sender, &receiver_before, sizeof(sender)) && wakeups == wakes_before + 1);
    master = true;
    // READY selects after the delivery tick. Its reply must not get ahead of
    // the old route's release, new activation, or fast continuing mouse input.
    clock_ms = 10000; submissions = 0;
    kms_delivery_init(&sender, replay, 99, submit, NULL, NULL);
    kms_delivery_select(&sender, 1, 111, clock_ms);
    kms_delivery_tick(&sender, clock_ms); acknowledge(clock_ms + 1);
    peers[1].record.valid = true; peers[1].boot = 123; peers[1].seen = clock_ms;
    wakes_before = wakeups;
    CHECK(kms_link_select(NULL, 2));
    CHECK(wakeups == wakes_before + 1 && route_starting(clock_ms));
    kms_delivery_input(&sender, &state, 40, 0, 0, 0, clock_ms);
    kms_delivery_tick(&sender, clock_ms);
    CHECK(submitted.type == KMS_RELEASE && route_starting(clock_ms));
    acknowledge(clock_ms + 1); kms_delivery_tick(&sender, clock_ms + 1);
    CHECK(submitted.type == KMS_ACTIVATE && route_starting(clock_ms + 1));
    acknowledge(clock_ms + 2);
    CHECK(route_starting(clock_ms + 2)); // First movement is still queued.
    kms_delivery_tick(&sender, clock_ms + 2);
    CHECK(kms_packet_is_input(&submitted) && submitted.motion[0] == 40 && route_starting(clock_ms + 2));
    acknowledge(clock_ms + 3);
    CHECK(submissions == 4 && !route_starting(clock_ms + 3));
    // An unreachable route cannot starve heartbeats or its READY reply.
    CHECK(kms_link_select(NULL, 2));
    CHECK(route_starting(clock_ms + KMS_RADIO_RETRY_WINDOW_MS - 1));
    CHECK(!route_starting(clock_ms + KMS_RADIO_RETRY_WINDOW_MS));
    // Clock wrap retains the same bound; receiver management is unaffected.
    last_selection = 0xfffffff0u;
    CHECK(route_starting(last_selection + KMS_RADIO_RETRY_WINDOW_MS - 1));
    CHECK(!route_starting(last_selection + KMS_RADIO_RETRY_WINDOW_MS));
    master = false; CHECK(!route_starting(last_selection)); master = true;
    return 0;
}
__declspec(dllexport) unsigned kms_link_receive_test_count(void) { return checks; }

static void recovery_fixture(bool active)
{
    memset(recovery_notices, 0, sizeof(recovery_notices)); recovery_notice_sequence = 0;
    memset(peers, 0, sizeof(peers)); memset(receiver_recovery, 0, sizeof(receiver_recovery));
    memset(recovery_initialized, 0, sizeof(recovery_initialized)); memset(recovery_errors, 0, sizeof(recovery_errors));
    memset(recovery_online, 0, sizeof(recovery_online));
    memset(recovery_reasons, 0, sizeof(recovery_reasons)); memset(recovery_seen_errors, 0, sizeof(recovery_seen_errors));
    memset(&saved_budget, 0, sizeof(saved_budget)); kms_delivery_init(&sender, replay, 99, NULL, NULL, NULL);
    master = true; auto_enabled = auto_supported = auto_confirmed = auto_queue = true;
    updating = channel_changing = pairing.open = transfer_busy = test_mode = radio_tx.blocked = radio_rejection_seen = false;
    channel_trial = channel_grace = manual_restart_pending = processing_fault_seen = false;
    channel_cancellations = channel_route.slot = 0;
    recovery_owner = recovery_resume_slot = paused_slot = restart_requests = route_epoch = 0;
    recovery_global_wait = result_pending = false; clock_ms = 10000;
    for (unsigned i = 0; i < KMS_SLOTS; i += 2) {
        peers[i].record.valid = true; peers[i].record.slot = (uint8_t)(i + 1);
        peers[i].record.mac[0] = 2; peers[i].record.mac[5] = (uint8_t)(i + 1);
        peers[i].boot = 100 + i; peers[i].seen = clock_ms; peers[i].flags = KMS_FLAG_USB_READY;
    }
    sender.slot = active ? 1 : 3; receiver_recovery_tick(clock_ms);
}
static void recovery_faults(uint32_t at, unsigned count)
{
    for (unsigned n = 0; n < count; ++n) {
        clock_ms = at + n * 700; peers[0].seen = peers[2].seen = clock_ms;
        ++recovery_errors[0]; recovery_reasons[0] = KMS_RECOVERY_MAC; receiver_recovery_tick(clock_ms);
    }
}
__declspec(dllexport) int kms_run_receiver_recovery_runtime_tests(void)
{
    checks = 0; recovery_fixture(false); recovery_faults(16000, 4);
    CHECK(restart_requests == 1 && requested_slot == 1 && !requested_active && sender.slot == 3);
    CHECK(requested_budget.attempts == 1 && requested_budget.level == 1 && recovery_owner == 1);
    recovery_fixture(true); recovery_faults(16000, 4); CHECK(!restart_requests);
    recovery_faults(18800, 8);
    CHECK(restart_requests == 1 && requested_active && !sender.slot && paused_slot == 1);
    peers[0].boot = 101; peers[0].seen = clock_ms; auto_confirmed = false;
    receiver_recovery_tick(++clock_ms); CHECK(!sender.slot && receiver_recovery[0].waiting);
    auto_confirmed = true; receiver_recovery_tick(++clock_ms);
    CHECK(sender.slot == 1 && !recovery_owner && receiver_recovery[0].attempts == 1);
    uint8_t notice[48]; CHECK(kms_link_recovery_status(1, notice) && kms_get_u32(notice + 12) == 1);
    CHECK(kms_get_u32(notice + 28) == 101 && kms_get_u32(notice + 36) == 1);
    CHECK(!kms_link_recovery_status(0, notice) && !kms_link_recovery_status(8, notice));
    // A manual selection or pause supersedes the recovery's temporary route.
    for (unsigned manual = 0; manual < 2; ++manual) {
        recovery_fixture(true); recovery_faults(16000, 12);
        if (manual) kms_link_pause(); else CHECK(kms_link_select(NULL, 3));
        peers[0].boot = 101; peers[0].seen = clock_ms; receiver_recovery_tick(++clock_ms);
        CHECK(sender.slot == (manual ? 0u : 3u) && !recovery_resume_slot);
    }
    // Every operational guard prevents scheduling, including sleeping USB hosts
    // and failures originating from the S3's local driver instead of the peer.
    for (unsigned guard = 0; guard < 14; ++guard) {
        recovery_fixture(false);
        switch (guard) {
        case 0: updating = true; break; case 1: channel_changing = true; break;
        case 2: pairing.open = true; break; case 3: test_mode = true; break;
        case 4: auto_enabled = false; break; case 5: auto_supported = false; break;
        case 6: auto_confirmed = false; break; case 7: peers[0].flags = 0; break;
        case 8: radio_tx.blocked = true; break; case 9: transfer_busy = true; break;
        case 10: radio_rejection_seen = true; radio_rejected_at = 15000; break;
        case 11: peers[0].suspended = true; break;
        case 12: manual_restart_pending = true; break;
        default: channel_grace = true; break;
        }
        recovery_faults(16000, 4); CHECK(!restart_requests);
    }
    recovery_fixture(true); auto_queue = false; recovery_faults(16000, 12);
    CHECK(restart_requests == 1 && sender.slot == 1 && !receiver_recovery[0].waiting);
    recovery_faults(25000, 8); CHECK(restart_requests == 1);
    recovery_fixture(true); recovery_faults(16000, 12);
    auto_result = (kms_recovery_result_t){.slot = 1, .boot = 100, .status = KMS_M_BUSY}; result_pending = true;
    receiver_recovery_tick(++clock_ms); CHECK(sender.slot == 1 && !recovery_owner);
    recovery_fixture(true); recovery_faults(16000, 12);
    clock_ms += KMS_RECOVERY_BOOT_WAIT_MS; receiver_recovery_tick(clock_ms);
    CHECK(!sender.slot && !recovery_owner && !recovery_resume_slot);
    CHECK(kms_link_recovery_status(1, notice) && kms_get_u32(notice + 12) == 2);
    recovery_fixture(false); saved_budget.attempts = KMS_RECOVERY_MAX_ATTEMPTS; saved_budget.level = 3;
    recovery_initialized[0] = false; receiver_recovery_tick(++clock_ms);
    recovery_faults(16000, 4); CHECK(!restart_requests);
    // A trial and its reconnect grace clear old evidence without touching budgets.
    recovery_fixture(false); receiver_recovery[0].attempts = 2;
    recovery_faults(16000, 3);
    channel_trial = true; recovery_faults(18100, 7);
    channel_trial = false; channel_grace = true; recovery_faults(23000, 7);
    CHECK(!restart_requests && receiver_recovery[0].attempts == 2 && !receiver_recovery[0].faults);
    channel_grace = false; recovery_faults(30000, 1);
    CHECK(!restart_requests && receiver_recovery[0].faults <= 1);
    recovery_faults(36100, 4); CHECK(restart_requests == 1 && requested_budget.attempts == 3);
    recovery_fixture(false);
    for (unsigned i = 0; i < 4; ++i) {
        processing_fault_seen = true; processing_fault_at = 16000 + i * 700;
        recovery_faults(processing_fault_at, 1);
    }
    CHECK(!restart_requests && !receiver_recovery[0].faults);
    return 0;
}
__declspec(dllexport) int kms_run_channel_route_tests(void)
{
    checks = 0;
    recovery_fixture(true);
    kms_state_t input = {0};
    kms_delivery_input(&sender, &input, 80, 0, 0, 0, clock_ms);
    CHECK(sender.count == 1);
    channel_trial = true; kms_link_channel_recovery_pause();
    CHECK(!sender.slot && !sender.count && sender.releasing && paused_slot == 1 && !channel_cancellations);
    channel_trial = false; channel_grace = true; clock_ms += 100;
    kms_link_channel_recovery_finish(true);
    channel_restore_route(clock_ms); CHECK(!sender.slot);
    peers[0].seen = ++clock_ms; peers[0].flags = 0;
    channel_restore_route(clock_ms); CHECK(!sender.slot);
    peers[0].flags = KMS_FLAG_USB_READY; channel_restore_route(clock_ms);
    CHECK(sender.slot == 1 && !sender.count && fault && !channel_route.slot);
    recovery_fixture(true); channel_trial = true; kms_link_channel_recovery_pause();
    CHECK(kms_link_select(NULL, 3) && channel_cancellations == 1 && !sender.slot);
    kms_link_channel_recovery_disown(); // Disable cannot erase a requested manual selection.
    channel_trial = false; kms_link_channel_recovery_finish(true);
    peers[2].seen = ++clock_ms; channel_restore_route(clock_ms); CHECK(sender.slot == 3);
    recovery_fixture(true); channel_trial = true; kms_link_channel_recovery_pause();
    kms_link_pause(); CHECK(channel_cancellations == 1);
    channel_trial = false; kms_link_channel_recovery_finish(true);
    peers[0].seen = ++clock_ms; channel_restore_route(clock_ms); CHECK(!sender.slot);
    recovery_fixture(true); channel_trial = true; kms_link_channel_recovery_pause();
    kms_link_channel_recovery_disown(); channel_trial = false; kms_link_channel_recovery_finish(true);
    peers[0].seen = ++clock_ms; channel_restore_route(clock_ms); CHECK(!sender.slot);
    return 0;
}
__declspec(dllexport) unsigned kms_receiver_recovery_runtime_test_count(void) { return checks; }
