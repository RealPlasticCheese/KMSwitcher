// Production router, delivery queue, receiver guard and deferred ACK owner.
// Only the two radio legs and their completion clock are simulated.
#include "kms_ack.h"
#include "kms_delivery.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)

typedef struct { kms_packet_t packet; uint32_t due; bool busy, lost; } flight_t;
static kms_delivery_t sender;
static kms_buffered_t replay[KMS_RECOVERY_BUFFER_DEPTH];
static kms_receiver_guard_t receiver;
static kms_ack_t receipt;
static kms_router_t router;
static flight_t input_flight, ack_flight;
static kms_state_t output;
static uint32_t now, start, one_way_ms, receipts_blocked_until, first_fault_at;
static unsigned checks, generated, movement, duplicates, peak, ordering_errors, lost_acks;
static unsigned events[KMS_EVENT_COUNT], dropped;
static bool check_order, fault_seen;

static kms_state_t state_at(unsigned index)
{
    kms_state_t state = {.buttons = (uint16_t)(1u << (index % 2))};
    kms_set_key(&state, 4 + index % 2, true);
    return state;
}
static void event(void *ctx, enum kms_event kind, unsigned slot, uint32_t detail)
{
    (void)ctx; (void)slot;
    ++events[kind];
    if (kind == KMS_EVENT_REPLAY_DROPPED) dropped += detail;
    if (!fault_seen && kind != KMS_EVENT_RECOVERED) {
        fault_seen = true; first_fault_at = now - start;
    }
}
static bool send_input(void *ctx, const kms_packet_t *packet)
{
    (void)ctx;
    if (input_flight.busy) return false;
    uint8_t wire[KMS_MAX_WIRE_SIZE]; kms_packet_t decoded;
    size_t bytes = kms_wire_encode(packet, wire, sizeof(wire));
    if (!bytes || !kms_wire_decode(&decoded, wire, bytes)) return false;
    input_flight = (flight_t){.packet = decoded, .due = now + one_way_ms, .busy = true};
    return true;
}
static bool send_ack(void *ctx, const kms_packet_t *packet)
{
    (void)ctx;
    if (ack_flight.busy || !kms_time_elapsed(now, receipts_blocked_until, 0)) return false;
    ack_flight = (flight_t){.packet = *packet, .due = now + one_way_ms, .busy = true, .lost = lost_acks != 0};
    if (lost_acks) --lost_acks;
    return true;
}
static void emit(void *ctx, const kms_state_t *state, int32_t x, int32_t y, int32_t wheel, int32_t pan)
{
    (void)ctx; kms_delivery_input(&sender, state, x, y, wheel, pan, now);
}
static void step(unsigned elapsed)
{
    now = start + elapsed;
    if (kms_receiver_expire(&receiver, now)) memset(&output, 0, sizeof(output));
    if (input_flight.busy && kms_time_elapsed(now, input_flight.due, 0)) {
        input_flight.busy = false;
        kms_sample_t samples[KMS_INPUT_BATCH_MAX]; unsigned count; bool apply;
        const kms_packet_t *packet = &input_flight.packet;
        if (kms_receiver_accept_many(&receiver, packet, now, samples, &count, &apply)) {
            if (apply) {
                for (unsigned i = 0; i < count; ++i) {
                    const kms_sample_t *sample = &samples[i];
                    output = sample->state;
                    if (kms_packet_is_input(packet) && sample->x) {
                        kms_state_t expected = state_at(movement);
                        if (check_order && (sample->x != 1 || !kms_state_equal(&sample->state, &expected)))
                            ++ordering_errors;
                        movement += (unsigned)sample->x;
                    }
                }
            } else ++duplicates;
            kms_ack_prepare(&receipt, packet);
            receipt.packet.flags = KMS_FLAG_USB_READY | (receiver.active ? KMS_FLAG_ACTIVE : 0);
        }
    }
    if (ack_flight.busy && kms_time_elapsed(now, ack_flight.due, 0)) {
        ack_flight.busy = false;
        if (!ack_flight.lost) kms_delivery_receive(&sender, &ack_flight.packet, now);
    }
    kms_ack_flush(&receipt, send_ack, NULL);
    kms_delivery_tick(&sender, now);
    if (sender.block) { sender.block = false; kms_router_block_output(&router); }
    if (sender.count > peak) peak = sender.count;
}
static void setup(uint32_t epoch, unsigned delay)
{
    memset(&receiver, 0, sizeof(receiver)); memset(&receipt, 0, sizeof(receipt));
    memset(&input_flight, 0, sizeof(input_flight)); memset(&ack_flight, 0, sizeof(ack_flight));
    memset(&output, 0, sizeof(output)); memset(events, 0, sizeof(events));
    start = now = receipts_blocked_until = epoch; one_way_ms = delay;
    generated = movement = duplicates = peak = ordering_errors = dropped = lost_acks = 0;
    fault_seen = false; check_order = true; first_fault_at = 0;
    kms_router_init(&router, emit, NULL, NULL, NULL);
    kms_delivery_init(&sender, replay, 42, send_input, event, NULL);
    kms_delivery_select(&sender, 1, 7, now);
    for (unsigned elapsed = 0; elapsed < 20; ++elapsed) step(elapsed);
}
static void input(unsigned elapsed)
{
    now = start + elapsed;
    kms_sample_t sample = {.state = state_at(generated), .x = 1};
    kms_router_input(&router, 0, &sample, now); ++generated;
}
static void neutral(unsigned elapsed)
{
    now = start + elapsed;
    kms_sample_t sample = {0}; kms_router_input(&router, 0, &sample, now);
}
static void workload(unsigned period)
{
    for (unsigned elapsed = 20; elapsed < 4020; ++elapsed) {
        if (elapsed % period == 0) input(elapsed);
        step(elapsed);
    }
    neutral(4020);
    for (unsigned elapsed = 4020; elapsed < 10020; ++elapsed) step(elapsed);
}

__declspec(dllexport) int kms_run_delivery_timing_tests(void)
{
    checks = 0;
    // The measured ~8 ms round trip can sustain 100 changing input reports/s.
    setup(0, 4); workload(10);
    CHECK(generated == 400 && movement == generated && !ordering_errors);
    CHECK(!fault_seen && !sender.count && !sender.has_pending && peak <= 1);
    kms_state_t zero = {0}; CHECK(kms_state_equal(&output, &zero));
    // Each generator emits a combined keyboard/mouse sample: the real
    // 100-cycle/s dual-generator workload offers 200 input samples/s.
    setup(0, 1); workload(5);
    CHECK(generated == 800 && movement == generated && !ordering_errors);
    CHECK(!fault_seen && !sender.count && !sender.has_pending && peak <= 2);
    CHECK(kms_state_equal(&output, &zero));
    // A fast mouse with unchanged held state can coalesce unsent motion even
    // when the same radio delay cannot carry every changing keyboard sample.
    setup(0, 4); check_order = false;
    for (unsigned elapsed = 20; elapsed < 4020; ++elapsed) {
        now = start + elapsed;
        kms_sample_t sample = {.x = 1};
        kms_router_input(&router, 0, &sample, now); ++generated;
        step(elapsed);
    }
    neutral(4020);
    for (unsigned elapsed = 4020; elapsed < 10020; ++elapsed) step(elapsed);
    CHECK(generated == 4000 && movement == generated);
    CHECK(!fault_seen && !sender.count && !sender.has_pending && peak <= 2);
    CHECK(kms_state_equal(&output, &zero));
    // With the measured delay, acknowledgements continue arriving while input
    // ages out and the finite queue overflows: reproduce the hardware failure.
    setup(0, 4); check_order = false; workload(5);
    CHECK(generated == 800 && movement < generated && fault_seen);
    CHECK(first_fault_at >= KMS_RADIO_MAX_AGE_MS && first_fault_at < 1000);
    CHECK(events[KMS_EVENT_ACK_TIMEOUT] && events[KMS_EVENT_TX_OVERFLOW]);
    CHECK(events[KMS_EVENT_REPLAY_DROPPED] && dropped && peak <= KMS_RECOVERY_BUFFER_DEPTH);
    CHECK(!sender.count && !sender.has_pending && kms_state_equal(&output, &zero));
    // The same measured delay must sustain the offered load once the receiver
    // explicitly advertises ordered, atomic input batches.
    setup(0, 4);
    kms_packet_t status = {.type = KMS_STATUS, .slot = 1, .session = 7, .receiver = 42,
        .generation = sender.generation, .sequence = receiver.sequence,
        .flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE};
    kms_batch_advertise(&status);
    kms_delivery_receive(&sender, &status, now);
    workload(5);
    CHECK(generated == 800 && movement == generated && !ordering_errors && !fault_seen);
    CHECK(!sender.count && !sender.has_pending && peak <= 4);
    CHECK(kms_state_equal(&output, &zero));
    // An 18 ms round trip is still fast enough for four changing samples per
    // batch at 200/s. Retrying before the first ACK arrives occupies the next
    // forward transmission slot and can turn that load into a growing backlog.
    setup(0, 9);
    status.generation = sender.generation; status.sequence = receiver.sequence;
    kms_delivery_receive(&sender, &status, now);
    workload(5);
    CHECK(generated == 800 && movement == generated && !ordering_errors && !fault_seen);
    CHECK(!sender.count && !sender.has_pending && duplicates == 0 && peak <= 8);
    CHECK(sender.ack_count > 0 && sender.ack_rtt_ms == 18 && sender.ack_max_ms == 18);
    CHECK(kms_state_equal(&output, &zero));
    // One lost receipt still retries within the original ACK budget, catches
    // up without dropping input, and works over the millisecond counter wrap.
    setup(0xffffffe0u, 9);
    status.generation = sender.generation; status.sequence = receiver.sequence;
    kms_delivery_receive(&sender, &status, now);
    lost_acks = 1;
    workload(5);
    CHECK(generated == 800 && movement == generated && !ordering_errors && !fault_seen);
    CHECK(!sender.count && !sender.has_pending && duplicates >= 1 && peak <= 12);
    CHECK(kms_state_equal(&output, &zero));
    // At the observed high-rate boundary, four inputs per ACK cannot sustain
    // 250 changing samples/s over an 18 ms round trip. A negotiated larger
    // batch must catch up without extending the stale-input deadline.
    setup(0, 9);
    status.generation = sender.generation; status.sequence = receiver.sequence;
    kms_delivery_receive(&sender, &status, now);
    workload(4);
    CHECK(generated == 1000 && movement == generated && !ordering_errors && !fault_seen);
    CHECK(!sender.count && !sender.has_pending && peak <= 12);
    CHECK(kms_state_equal(&output, &zero));
    // Delayed and lost ACKs must not duplicate a buffered key/button/motion
    // burst. After the delay the exact stream and final neutral state survive.
    setup(0, 4); receipts_blocked_until = start + 120; lost_acks = 2;
    for (unsigned elapsed = 20; elapsed < 420; ++elapsed) {
        if (elapsed % 10 == 0) input(elapsed);
        step(elapsed);
    }
    neutral(420);
    for (unsigned elapsed = 420; elapsed < 2500; ++elapsed) step(elapsed);
    CHECK(generated == 40 && movement == generated && !ordering_errors && duplicates >= 2);
    CHECK(events[KMS_EVENT_ACK_TIMEOUT] && events[KMS_EVENT_RECOVERED] && !dropped);
    CHECK(!sender.count && !sender.has_pending && kms_state_equal(&output, &zero));
    // Batch recovery must catch up after a temporary ACK outage while the
    // generators continue, without duplicating any state change or movement.
    setup(0, 4);
    status.generation = sender.generation; status.sequence = receiver.sequence;
    kms_delivery_receive(&sender, &status, now);
    receipts_blocked_until = start + 120; lost_acks = 2;
    for (unsigned elapsed = 20; elapsed < 4020; ++elapsed) {
        if (elapsed % 5 == 0) input(elapsed);
        step(elapsed);
    }
    CHECK(sender.count <= 4 && !sender.recovering);
    neutral(4020);
    for (unsigned elapsed = 4020; elapsed < 10020; ++elapsed) step(elapsed);
    CHECK(generated == 800 && movement == generated && !ordering_errors);
    CHECK(events[KMS_EVENT_ACK_TIMEOUT] && events[KMS_EVENT_RECOVERED] && !dropped && duplicates >= 2);
    CHECK(!sender.count && !sender.has_pending && kms_state_equal(&output, &zero));
    // The same asynchronous workload works across the 32-bit millisecond wrap.
    setup(0xfffffff0u, 4); workload(10);
    CHECK(generated == 400 && movement == generated && !ordering_errors && !fault_seen);
    CHECK(!sender.count && kms_state_equal(&output, &zero));
    return 0;
}
__declspec(dllexport) unsigned kms_delivery_timing_test_count(void) { return checks; }
