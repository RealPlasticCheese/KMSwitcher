// Production delivery state machine connected to the production receiver guard.
#include "kms_delivery.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static kms_delivery_t d;
static kms_buffered_t replay[KMS_RECOVERY_BUFFER_DEPTH];
static kms_receiver_guard_t receiver;
static kms_diag_model_t diagnostics;
static kms_sample_t applied[1024];
static unsigned checks, applied_count, notices[KMS_EVENT_COUNT], transmissions;
static uint32_t now, receiver_boot;
static bool lose_radio, lose_ack;
static void event(void *ctx, enum kms_event e, unsigned slot, uint32_t detail)
{ (void)ctx; (void)slot; (void)detail; ++notices[e]; }
static bool send(void *ctx, const kms_packet_t *p)
{
    (void)ctx; ++transmissions;
    if (lose_radio || p->slot != 1 || p->receiver != receiver_boot) return true;
    kms_sample_t sample; bool deliver;
    if (!kms_receiver_accept(&receiver, p, now, &sample, &deliver)) return true;
    if (deliver && applied_count < 1024) applied[applied_count++] = sample;
    if (lose_ack) return true;
    kms_packet_t ack = *p; ack.type = KMS_ACK;
    ack.flags = KMS_FLAG_USB_READY | (receiver.active ? KMS_FLAG_ACTIVE : 0);
    kms_delivery_receive(&d, &ack, now); return true;
}
static void tick(uint32_t at)
{ now = at; (void)kms_receiver_expire(&receiver, now); kms_delivery_tick(&d, now); }
static void status(void)
{
    kms_packet_t p = {.type = KMS_STATUS, .slot = 1, .session = receiver_boot,
        .receiver = receiver.session, .generation = receiver.generation, .sequence = receiver.sequence,
        .flags = KMS_FLAG_USB_READY | (receiver.active ? KMS_FLAG_ACTIVE : 0)};
    kms_delivery_receive(&d, &p, now);
}
static void setup(void)
{
    memset(&receiver, 0, sizeof(receiver)); memset(notices, 0, sizeof(notices));
    applied_count = transmissions = 0; lose_radio = lose_ack = false; now = 10; receiver_boot = 7;
    kms_delivery_init(&d, replay, 42, send, event, NULL); kms_delivery_select(&d, 1, receiver_boot, now); tick(now);
    applied_count = 0;
}
static void input(unsigned key, int32_t x, uint32_t at)
{
    kms_state_t state = {0}; if (key) kms_set_key(&state, key, true);
    now = at; kms_delivery_input(&d, &state, x, 0, 0, 0, now);
}
__declspec(dllexport) int kms_run_recovery_tests(void)
{
    checks = 0; setup();
    CHECK(receiver.active && !d.has_pending && d.slot == 1);
    input(4, 0, 20); tick(20); input(0, 3, 21); tick(21);
    CHECK(applied_count == 2 && kms_key(&applied[0].state, 4));
    CHECK(!kms_key(&applied[1].state, 4) && applied[1].x == 3 && !d.count);
    // Input ACKs prove contact even when periodic receiver status is delayed.
    setup();
    for (uint32_t at = 20; at < KMS_PEER_TIMEOUT_MS * 3; at += KMS_SNAPSHOT_MS) {
        input(0, 1, at); tick(at);
        CHECK(!d.recovering && !d.count && !d.has_pending);
    }
    CHECK(notices[KMS_EVENT_PEER_LOST] == 0 && notices[KMS_EVENT_RECOVERED] == 0);
    CHECK(d.seen == now);
    // Stale or mismatched ACKs cannot extend the selected receiver's lifetime.
    setup(); lose_ack = true; input(4, 0, 20); tick(20);
    kms_packet_t ack = d.pending; ack.type = KMS_ACK;
    ack.flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE;
    kms_packet_t invalid[5];
    for (unsigned i = 0; i < 5; ++i) invalid[i] = ack;
    ++invalid[0].slot; ++invalid[1].session; ++invalid[2].receiver;
    ++invalid[3].generation; ++invalid[4].sequence;
    uint32_t seen = d.seen;
    for (unsigned i = 0; i < 5; ++i) {
        kms_delivery_receive(&d, &invalid[i], 30 + i);
        CHECK(d.seen == seen && d.has_pending);
    }
    kms_delivery_receive(&d, &ack, 40);
    CHECK(d.seen == 40 && !d.has_pending && !d.count);
    kms_delivery_receive(&d, &ack, 50);
    CHECK(d.seen == 40);
    // Genuine silence after the last accepted ACK still expires at the deadline.
    lose_radio = true; tick(40 + KMS_PEER_TIMEOUT_MS - 1);
    CHECK(notices[KMS_EVENT_PEER_LOST] == 0);
    tick(40 + KMS_PEER_TIMEOUT_MS);
    CHECK(d.recovering && notices[KMS_EVENT_PEER_LOST] == 1);
    // Lost radio: both key edges, mouse movement and their order survive recovery.
    setup(); lose_radio = true;
    input(4, 0, 20); tick(20); input(0, 7, 21);
    tick(20 + KMS_RADIO_RETRY_WINDOW_MS);
    CHECK(d.recovering && d.slot == 1 && notices[KMS_EVENT_ACK_TIMEOUT] == 1);
    tick(200); CHECK(notices[KMS_EVENT_ACK_TIMEOUT] == 1 && d.count == 2);
    now = KMS_LEASE_MS + 30; (void)kms_receiver_expire(&receiver, now); lose_radio = false; status();
    tick(now); tick(now + KMS_RECOVERY_REPLAY_MS); tick(now + KMS_RECOVERY_REPLAY_MS);
    CHECK(!d.recovering && notices[KMS_EVENT_RECOVERED] == 1 && !d.count);
    CHECK(applied_count == 3 && kms_key(&applied[1].state, 4));
    CHECK(!kms_key(&applied[2].state, 4) && applied[2].x == 7);
    // Receiver status resolves an ACK lost after accepting a click/key.
    setup(); lose_ack = true; input(5, 9, 20); tick(20);
    CHECK(applied_count == 1 && d.count == 1);
    tick(100); CHECK(d.recovering && applied_count == 1);
    lose_ack = false; status(); CHECK(!d.recovering && !d.count && applied_count == 1);
    tick(110); CHECK(applied_count == 1);
    // A delayed inactive status older than the confirmed packet is ignored.
    kms_packet_t delayed = {.type = KMS_STATUS, .slot = 1, .session = receiver_boot,
        .receiver = d.boot, .generation = d.generation, .sequence = d.acknowledged_sequence - 1,
        .flags = KMS_FLAG_USB_READY};
    uint32_t generation = d.generation;
    kms_delivery_receive(&d, &delayed, 111);
    CHECK(d.generation == generation && !d.recovering && !d.has_pending);
    // A receive-queue fault with no user input recovers using an automatic probe.
    setup(); kms_delivery_fault(&d, KMS_EVENT_RX_OVERFLOW, 20); tick(20);
    CHECK(!d.recovering && notices[KMS_EVENT_RECOVERED] == 1);
    // A duplicate ACK received after the lease expires requires activation first.
    setup(); lose_ack = true; input(5, 0, 20); tick(20); tick(100);
    lose_ack = false; tick(KMS_LEASE_MS + 30);
    CHECK(d.need_activate && d.recovering);
    tick(KMS_LEASE_MS + 40); CHECK(!d.recovering);
    // A receiver restart cannot prove delivery of the one in-flight event.
    setup(); lose_ack = true; input(6, 11, 20); tick(20); input(0, 0, 21);
    ++receiver_boot; memset(&receiver, 0, sizeof(receiver)); lose_ack = false; now = 30; status();
    CHECK(notices[KMS_EVENT_REPLAY_DROPPED] == 1 && d.count == 1);
    tick(30); tick(40);
    CHECK(!d.recovering && !d.count && applied_count == 3);
    CHECK(!kms_key(&applied[2].state, 6) && !applied[2].x);
    // A still-live old master lease is retried; selection never falls to another PC.
    setup(); receiver.session = 123; receiver.last_seen = now; receiver.active = true;
    kms_delivery_init(&d, replay, 42, send, event, NULL);
    kms_delivery_select(&d, 1, receiver_boot, 20); input(7, 0, 21); input(0, 0, 22);
    tick(23); tick(23 + KMS_RADIO_RETRY_WINDOW_MS); CHECK(d.recovering && d.slot == 1);
    tick(500); status(); CHECK(d.slot == 1 && d.recovering);
    tick(KMS_LEASE_MS + 30); tick(KMS_LEASE_MS + 40); tick(KMS_LEASE_MS + 50);
    CHECK(!d.recovering && d.slot == 1 && !d.count);
    // Fixed capacity: never replay a truncated chord; suppress the rest of this outage.
    setup(); lose_radio = true;
    for (unsigned i = 0; i < KMS_RECOVERY_BUFFER_DEPTH + 1; ++i) input(i % 2 ? 0 : 8, 0, 20 + i);
    CHECK(d.suppressed && d.block && !d.count && d.recovering);
    CHECK(notices[KMS_EVENT_REPLAY_DROPPED] == 1);
    kms_delivery_fault(&d, KMS_EVENT_RX_OVERFLOW, 22);
    kms_delivery_fault(&d, KMS_EVENT_RX_OVERFLOW, 23);
    CHECK(notices[KMS_EVENT_RX_OVERFLOW] == 1);
    input(9, 0, 400); CHECK(!d.count);
    now = KMS_LEASE_MS + 50; (void)kms_receiver_expire(&receiver, now); lose_radio = false; status(); tick(now);
    CHECK(!d.recovering && !d.suppressed && d.block && !d.count);
    // A lost USB report invalidates the backlog even if its capacity is available.
    setup(); input(8, 0, 20);
    kms_delivery_abandon(&d, KMS_EVENT_INPUT_OVERFLOW, 21);
    CHECK(d.suppressed && d.block && !d.count && notices[KMS_EVENT_INPUT_OVERFLOW] == 1);
    CHECK(notices[KMS_EVENT_REPLAY_DROPPED] == 1);
    // Age bound: a long outage still reconnects but old input does not execute.
    setup(); lose_radio = true; input(9, 0, 20); tick(20); tick(100);
    tick(20 + KMS_RECOVERY_BUFFER_MS);
    CHECK(d.suppressed && !d.count && notices[KMS_EVENT_REPLAY_DROPPED] == 1);
    input(0, 100, now + 1);
    lose_radio = false; status(); tick(now);
    CHECK(!d.recovering && !d.count && applied_count == 1);
    input(0, 1, now + 10); tick(now);
    CHECK(applied_count == 2 && applied[1].x == 1);
    // USB suspend and resume recover automatically while preserving new edges.
    setup(); now = 20;
    kms_packet_t suspended = {.type = KMS_STATUS, .slot = 1, .session = receiver_boot,
        .receiver = d.boot, .generation = d.generation, .sequence = receiver.sequence};
    receiver.active = false; kms_delivery_receive(&d, &suspended, now);
    input(12, 0, 21); input(0, 0, 22);
    CHECK(d.recovering && d.count == 2);
    now = 40; status(); tick(40); tick(50); tick(60);
    CHECK(!d.recovering && !d.count && kms_key(&applied[1].state, 12));
    // An explicit destination change discards input belonging to the old computer.
    setup(); lose_radio = true; input(10, 5, 20); tick(20); tick(100);
    kms_delivery_select(&d, 2, 99, 110);
    CHECK(d.slot == 2 && !d.count && !d.recovering && d.pending.type == KMS_ACTIVATE);
    kms_packet_t stale = {.type = KMS_ACK, .slot = 1, .session = 42, .receiver = 7, .generation = 1, .sequence = 2};
    kms_delivery_receive(&d, &stale, 111); CHECK(d.has_pending && d.slot == 2);
    // Switching retries a lost old-PC release before activating the next one.
    setup(); lose_ack = true; kms_delivery_select(&d, 2, 99, 20); tick(20);
    CHECK(d.releasing && !receiver.active && !d.started);
    unsigned sent = transmissions; tick(20 + KMS_RADIO_RETRY_MS);
    CHECK(d.releasing && transmissions == sent + 1);
    tick(20 + KMS_RADIO_RETRY_WINDOW_MS);
    CHECK(!d.releasing && d.started && d.pending.slot == 2);
    // USB-host recovery clears stale input through a new generation on the same
    // PC, including a release before activation. No old movement is replayed.
    setup(); input(4, 0, 20); tick(20); lose_radio = true;
    input(5, 99, 21); tick(21); input(6, 100, 22);
    CHECK(d.count > 0 && kms_key(&applied[0].state, 4));
    generation = d.generation; unsigned before_reset = applied_count;
    kms_delivery_select(&d, d.slot, d.receiver, 30); lose_radio = false;
    tick(30); tick(40); tick(50);
    CHECK(d.slot == 1 && d.generation != generation && !d.count && receiver.active);
    CHECK(applied_count > before_reset);
    for (unsigned n = before_reset; n < applied_count; ++n)
        CHECK(!kms_key(&applied[n].state, 4) && !kms_key(&applied[n].state, 5) && !kms_key(&applied[n].state, 6) && !applied[n].x);
    // Pointer coalescing never merges the immutable in-flight sample.
    setup(); lose_radio = true; input(0, 1, 20); tick(20);
    input(0, 2, 21); input(0, 3, 22);
    CHECK(d.count == 2 && d.pending.motion[0] == 1 && d.buffer[(d.head + 1) % KMS_RECOVERY_BUFFER_DEPTH].motion[0] == 6);
    // Recovery timers work through the 32-bit clock wrap.
    setup(); now = 0xfffffff0u; kms_delivery_init(&d, replay, 42, send, event, NULL);
    kms_delivery_select(&d, 1, receiver_boot, now);
    lose_radio = true; input(11, 0, now); tick(now); tick(now + KMS_RADIO_RETRY_WINDOW_MS);
    CHECK(d.recovering && notices[KMS_EVENT_ACK_TIMEOUT] == 1);
    // Statistics preserve idle context, count repeats and checkpoint once per hour.
    CHECK(!memcmp(kms_diag_event_name(KMS_EVENT_INPUT_ERROR, KMS_USB_HOST_EVENT_TAG | KMS_USB_HOST_RESTORED << 16), "USB host recovered", 18));
    CHECK(kms_diag_event_name(KMS_EVENT_INPUT_ERROR, 0x54010000u) == kms_event_name(KMS_EVENT_INPUT_ERROR));
    CHECK(kms_diag_event_name(KMS_EVENT_BOOT, KMS_USB_HOST_EVENT_TAG | 3u << 16) == kms_event_name(KMS_EVENT_BOOT));
    CHECK(kms_diag_event_name(KMS_EVENT_INPUT_ERROR, KMS_USB_HOST_EVENT_TAG) == kms_event_name(KMS_EVENT_INPUT_ERROR));
    kms_diag_model_init(&diagnostics, 0, 55, 3);
    kms_diag_model_tick(&diagnostics, KMS_DIAGNOSTICS_IDLE_MS + 100);
    CHECK(diagnostics.stats.idle_ms == 100);
    unsigned index = kms_diag_model_event(&diagnostics, KMS_DIAGNOSTICS_IDLE_MS + 100, KMS_EVENT_ACK_TIMEOUT, 3, 2);
    CHECK(diagnostics.records[index].idle_ms == KMS_DIAGNOSTICS_IDLE_MS + 100);
    CHECK(kms_diag_model_event(&diagnostics, KMS_DIAGNOSTICS_IDLE_MS + 101, KMS_EVENT_ACK_TIMEOUT, 3, 2) == index);
    CHECK(diagnostics.records[index].repeats == 2 && diagnostics.stats.events[KMS_EVENT_ACK_TIMEOUT] == 2);
    CHECK(!kms_diag_model_checkpoint(&diagnostics, KMS_DIAGNOSTICS_STATS_MS - 1));
    CHECK(kms_diag_model_checkpoint(&diagnostics, KMS_DIAGNOSTICS_STATS_MS));
    CHECK(!kms_diag_model_checkpoint(&diagnostics, KMS_DIAGNOSTICS_STATS_MS + 1));
    kms_diag_model_input(&diagnostics, KMS_DIAGNOSTICS_STATS_MS + 2, true, true);
    uint64_t idle = diagnostics.stats.idle_ms;
    kms_diag_model_tick(&diagnostics, KMS_DIAGNOSTICS_STATS_MS + 20000);
    CHECK(diagnostics.stats.idle_ms == idle);
    for (unsigned i = 0; i < KMS_DIAGNOSTICS_EVENTS + 1; ++i)
        (void)kms_diag_model_event(&diagnostics, KMS_DIAGNOSTICS_STATS_MS + 20001 + i, KMS_EVENT_SELECTED, i, i);
    CHECK(diagnostics.next < KMS_DIAGNOSTICS_EVENTS && diagnostics.next_sequence == KMS_DIAGNOSTICS_EVENTS + 2);
    kms_diag_model_init(&diagnostics, 0xfffffff0u, 55, 3);
    kms_diag_model_tick(&diagnostics, 20); CHECK(diagnostics.stats.uptime_ms == 36);
    return 0;
}
__declspec(dllexport) int kms_recovery_test_count(void) { return checks; }
