#include "kms_delivery.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)

typedef struct {
    uint32_t id, generation, sequence;
    unsigned page, usage, slot, source;
    enum kms_key_trace_stage stage;
    enum kms_key_trace_reason reason;
    bool down;
} record_t;
static record_t records[8192];
static unsigned recorded, checks;
static kms_delivery_t sender;
static kms_buffered_t replay[KMS_RECOVERY_BUFFER_DEPTH];
static kms_router_t router;
static kms_packet_t sent;
static uint32_t now;
static bool radio_available;
static void change(void *ctx, unsigned page, unsigned usage, bool down)
{
    const kms_key_trace_t *trace = ctx;
    if (recorded >= sizeof(records) / sizeof(*records)) return;
    records[recorded++] = (record_t){.id = trace->id, .generation = trace->generation,
        .sequence = trace->sequence, .slot = trace->slot, .source = trace->source,
        .stage = trace->stage, .reason = trace->reason, .page = page, .usage = usage, .down = down};
}
static void trace(void *ctx, const kms_key_trace_t *event)
{ (void)ctx; kms_key_changes(event->before, event->after, change, (void *)event); }
static unsigned count(enum kms_key_trace_stage stage, int reason)
{
    unsigned total = 0;
    for (unsigned n = 0; n < recorded; ++n)
        if (records[n].stage == stage && (reason < 0 || records[n].reason == (enum kms_key_trace_reason)reason)) ++total;
    return total;
}
static bool send(void *ctx, const kms_packet_t *packet)
{ (void)ctx; if (!radio_available) return false; sent = *packet; return true; }
static void receipt(void)
{
    kms_packet_t ack = {.type = KMS_ACK, .slot = sent.slot, .session = sent.session,
        .receiver = sent.receiver, .generation = sent.generation, .sequence = sent.sequence,
        .flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE};
    kms_delivery_receive(&sender, &ack, ++now);
}
static void emit(void *ctx, const kms_state_t *s, int32_t x, int32_t y, int32_t wheel, int32_t pan)
{ (void)ctx; kms_delivery_input(&sender, s, x, y, wheel, pan, now); }
static bool select_slot(void *ctx, unsigned slot) { (void)ctx; (void)slot; return true; }
static void setup(void)
{
    recorded = 0; now = 100; radio_available = true;
    kms_delivery_init(&sender, replay, 42, send, NULL, NULL); sender.trace = trace;
    kms_delivery_select(&sender, 2, 7, now); kms_delivery_tick(&sender, now); receipt();
    kms_router_init(&router, emit, select_slot, NULL, NULL); router.trace = trace;
}
static void key(unsigned source, unsigned usage)
{
    kms_sample_t sample = {0}; kms_set_key(&sample.state, usage, true);
    kms_router_input(&router, source, &sample, ++now);
}
static void neutral(unsigned source)
{ kms_sample_t sample = {0}; kms_router_input(&router, source, &sample, ++now); }

__declspec(dllexport) int kms_run_key_trace_tests(void)
{
    checks = 0; setup(); key(0, 4); neutral(0);
    CHECK(count(KMS_KEY_OBSERVED, -1) == 2 && count(KMS_KEY_QUEUED, -1) == 2);
    uint32_t first_id = records[2].id;
    CHECK(first_id && records[2].slot == 2);
    kms_delivery_tick(&sender, ++now);
    CHECK(count(KMS_KEY_SUBMITTED, -1) == 1 && !count(KMS_KEY_CONFIRMED, -1));
    kms_packet_t wrong = {.type = KMS_ACK, .slot = sent.slot, .session = sent.session,
        .receiver = sent.receiver, .generation = sent.generation, .sequence = sent.sequence + 1};
    kms_delivery_receive(&sender, &wrong, ++now);
    CHECK(!count(KMS_KEY_CONFIRMED, -1));
    now += KMS_RADIO_RETRY_MS; kms_delivery_tick(&sender, now);
    CHECK(count(KMS_KEY_SUBMITTED, -1) == 1); // A retransmission is the same key event.
    receipt();
    CHECK(count(KMS_KEY_CONFIRMED, -1) == 1 && records[recorded - 1].id == first_id);
    CHECK(records[recorded - 1].sequence == sent.sequence && records[recorded - 1].down);
    kms_delivery_tick(&sender, ++now); receipt();
    CHECK(count(KMS_KEY_CONFIRMED, -1) == 2 && !records[recorded - 1].down);
    CHECK(!count(KMS_KEY_MISSING, -1) && !count(KMS_KEY_UNCONFIRMED, -1));

    // A negotiated batch retains every intermediate press and release, not just
    // its final neutral snapshot. An authenticated status can replace a lost ACK.
    setup(); sender.batch_limit = KMS_INPUT_BATCH_MAX;
    key(0, 4); neutral(0); key(0, 5); neutral(0);
    kms_delivery_tick(&sender, ++now); CHECK(sent.type == KMS_INPUT_BATCH && sender.pending_count == 4);
    kms_packet_t status = {.type = KMS_STATUS, .slot = 2, .session = 7, .receiver = 42,
        .generation = sent.generation, .sequence = sent.sequence, .flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE};
    kms_delivery_receive(&sender, &status, ++now);
    CHECK(count(KMS_KEY_CONFIRMED, -1) == 4 && !sender.count);
    kms_delivery_receive(&sender, &status, ++now);
    CHECK(count(KMS_KEY_CONFIRMED, -1) == 4);

    // Unsubmitted keys are missing; submitted keys with no receipt are unknown.
    setup(); key(0, 4); kms_delivery_tick(&sender, ++now); neutral(0);
    kms_delivery_stop(&sender);
    CHECK(count(KMS_KEY_UNCONFIRMED, KMS_KEY_ROUTE_STOPPED) == 1);
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_ROUTE_STOPPED) == 1 && !records[recorded - 1].down);
    CHECK(!count(KMS_KEY_CONFIRMED, -1));
    key(0, 5); CHECK(count(KMS_KEY_MISSING, KMS_KEY_NO_ROUTE) == 1);

    setup(); key(0, 4); kms_delivery_tick(&sender, ++now); neutral(0);
    status.session = 8; status.generation = sender.generation; status.sequence = 0;
    kms_delivery_receive(&sender, &status, ++now);
    CHECK(count(KMS_KEY_UNCONFIRMED, KMS_KEY_RECEIVER_RESTART) == 1 && sender.count == 1);
    CHECK(!count(KMS_KEY_CONFIRMED, -1));
    kms_delivery_stop(&sender);
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_ROUTE_STOPPED) == 1);

    // Replacing an in-flight packet for reactivation must preserve uncertainty.
    setup(); key(0, 4); kms_delivery_tick(&sender, ++now);
    status.session = 7; status.generation = sender.generation;
    status.sequence = sender.acknowledged_sequence; status.flags = KMS_FLAG_USB_READY;
    kms_delivery_receive(&sender, &status, ++now); CHECK(sender.pending.type == KMS_ACTIVATE);
    kms_delivery_stop(&sender); CHECK(count(KMS_KEY_UNCONFIRMED, KMS_KEY_ROUTE_STOPPED) == 1);

    // A smaller retry batch can retire part of an earlier submitted prefix.
    // The remaining entries keep the old sequence, and fresh input has none.
    setup(); sender.batch_limit = KMS_INPUT_BATCH_MAX;
    key(0, 4); neutral(0); key(0, 5); neutral(0);
    kms_delivery_tick(&sender, ++now);
    CHECK(sent.type == KMS_INPUT_BATCH && sender.pending_count == 4);
    uint32_t prior_sequence = sent.sequence, prior_generation = sent.generation;
    status.generation = prior_generation; status.sequence = sender.acknowledged_sequence;
    kms_delivery_receive(&sender, &status, ++now);
    CHECK(sender.pending.type == KMS_ACTIVATE && sender.batch_limit == 1);
    kms_delivery_tick(&sender, ++now); receipt();
    now += KMS_RECOVERY_REPLAY_MS; kms_delivery_tick(&sender, now);
    CHECK(sent.type == KMS_INPUT && sent.generation != prior_generation);
    receipt(); CHECK(sender.count == 3);
    key(0, 6); kms_delivery_stop(&sender);
    for (unsigned n = recorded - 4; n < recorded - 1; ++n) {
        CHECK(records[n].stage == KMS_KEY_UNCONFIRMED && records[n].sequence == prior_sequence);
        CHECK(records[n].generation == prior_generation);
    }
    CHECK(records[recorded - 1].stage == KMS_KEY_MISSING && !records[recorded - 1].sequence);

    setup(); radio_available = false; key(0, 4); kms_delivery_tick(&sender, ++now);
    now += KMS_RADIO_MAX_AGE_MS; kms_delivery_tick(&sender, now);
    sender.seen = now + KMS_RECOVERY_BUFFER_MS; now += KMS_RECOVERY_BUFFER_MS;
    kms_delivery_tick(&sender, now);
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_RADIO_BUSY) == 1 && !count(KMS_KEY_SUBMITTED, -1));
    neutral(0); CHECK(count(KMS_KEY_MISSING, KMS_KEY_SUPPRESSED) == 1);
    neutral(0); CHECK(count(KMS_KEY_MISSING, KMS_KEY_SUPPRESSED) == 1);

    setup(); key(0, 4); kms_delivery_tick(&sender, ++now); neutral(0);
    now += KMS_RADIO_RETRY_WINDOW_MS; kms_delivery_tick(&sender, now);
    sender.seen = now + KMS_RECOVERY_BUFFER_MS; now += KMS_RECOVERY_BUFFER_MS;
    kms_delivery_tick(&sender, now);
    CHECK(count(KMS_KEY_UNCONFIRMED, KMS_KEY_ACK_TIMEOUT) == 1);
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_ACK_TIMEOUT) == 1);
    setup(); key(0, 4); kms_delivery_tick(&sender, ++now);
    now += KMS_RADIO_RETRY_WINDOW_MS; kms_delivery_tick(&sender, now); receipt();
    CHECK(count(KMS_KEY_CONFIRMED, -1) == 1 && !count(KMS_KEY_MISSING, -1) && !count(KMS_KEY_UNCONFIRMED, -1));
    setup(); key(0, 4);
    status = (kms_packet_t){.type = KMS_STATUS, .slot = 2, .session = 7, .receiver = 42,
        .generation = sender.generation, .sequence = sender.acknowledged_sequence};
    kms_delivery_receive(&sender, &status, ++now);
    now += KMS_RECOVERY_BUFFER_MS; sender.seen = now; kms_delivery_tick(&sender, now);
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_USB_UNAVAILABLE) == 1);

    // Include the incoming event that could not fit, and every queued release.
    setup(); radio_available = false;
    for (unsigned n = 0; n <= KMS_RECOVERY_BUFFER_DEPTH; ++n) { if (n % 2) neutral(0); else key(0, 4); }
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_BUFFER_FULL) == KMS_RECOVERY_BUFFER_DEPTH + 1);
    CHECK(!sender.count && sender.suppressed);
    setup(); key(0, 4); kms_delivery_abandon(&sender, KMS_EVENT_INPUT_OVERFLOW, ++now);
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_INPUT_OVERFLOW) == 1);

    // Local triggers are deferred, then replayed or explicitly consumed.
    setup(); kms_router_set_trigger(&router, KMS_TRIGGER_INSERT); recorded = 0;
    key(0, KMS_HID_KEY_INSERT); neutral(0);
    CHECK(count(KMS_KEY_DEFERRED, -1) == 2 && !count(KMS_KEY_QUEUED, -1));
    now += KMS_DOUBLE_MS; kms_router_tick(&router, now);
    CHECK(count(KMS_KEY_QUEUED, -1) == 2);
    setup(); kms_router_set_trigger(&router, KMS_TRIGGER_INSERT); recorded = 0;
    key(0, KMS_HID_KEY_INSERT); neutral(0); kms_router_remove(&router, 0);
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_SOURCE_REMOVED) == 2);
    setup(); kms_router_set_trigger(&router, KMS_TRIGGER_INSERT); recorded = 0;
    key(0, KMS_HID_KEY_INSERT); neutral(0); kms_router_block_output(&router);
    CHECK(count(KMS_KEY_MISSING, KMS_KEY_HELD_BLOCKED) == 2);
    kms_router_block_output(&router); CHECK(count(KMS_KEY_MISSING, KMS_KEY_HELD_BLOCKED) == 2);
    setup(); kms_router_set_trigger(&router, KMS_TRIGGER_INSERT); recorded = 0;
    key(0, KMS_HID_KEY_INSERT); neutral(0); key(0, KMS_HID_KEY_INSERT); neutral(0);
    key(0, KMS_HID_KEY_1); neutral(0);
    CHECK(count(KMS_KEY_CONSUMED, KMS_KEY_SHORTCUT) == 6);
    CHECK(!count(KMS_KEY_QUEUED, -1) && !count(KMS_KEY_MISSING, -1));
    setup(); key(0, 4); key(1, 4); neutral(0); neutral(1);
    CHECK(count(KMS_KEY_CONSUMED, KMS_KEY_MERGED) == 2);
    CHECK(count(KMS_KEY_QUEUED, -1) == 2);
    setup(); router.diagnostic_typing = true; key(0, 4);
    CHECK(count(KMS_KEY_CONSUMED, KMS_KEY_CONSOLE) == 1 && !count(KMS_KEY_QUEUED, -1));

    // Modifiers, media and system keys retain page/usage and key-up identity;
    // reordering a consumer array does not invent transitions.
    setup(); kms_state_t zero = {0}, controls = {.modifiers = 0x81, .system = 5, .consumer = {0xe9, 0xea}};
    kms_key_trace_emit(trace, NULL, KMS_KEY_QUEUED, KMS_KEY_OK, 0, 2, 1, 1, 0, &zero, &controls);
    CHECK(recorded == 6);
    kms_state_t reordered = controls; reordered.consumer[0] = 0xea; reordered.consumer[1] = 0xe9;
    kms_key_trace_emit(trace, NULL, KMS_KEY_QUEUED, KMS_KEY_OK, 0, 2, 2, 1, 0, &controls, &reordered);
    CHECK(recorded == 6);
    kms_key_trace_emit(trace, NULL, KMS_KEY_MISSING, KMS_KEY_USB_QUEUE_FULL, 1, 2, 0, 0, 0, &controls, &zero);
    CHECK(recorded == 12 && count(KMS_KEY_MISSING, KMS_KEY_USB_QUEUE_FULL) == 6);
    for (unsigned n = 6; n < 12; ++n) CHECK(!records[n].down && records[n].slot == 2);
    CHECK(!memcmp(kms_key_trace_stage_name(KMS_KEY_CONFIRMED), "confirmed", sizeof("confirmed")));
    CHECK(!memcmp(kms_key_trace_reason_name(KMS_KEY_USB_STALE), "usb-report-too-old", sizeof("usb-report-too-old")));
    return 0;
}
__declspec(dllexport) unsigned kms_key_trace_test_count(void) { return checks; }
