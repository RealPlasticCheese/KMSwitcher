#include "kms_delivery.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, dropped;
static kms_delivery_t sender;
static kms_buffered_t replay[KMS_RECOVERY_BUFFER_DEPTH];
static kms_packet_t sent;
static bool send(void *ctx, const kms_packet_t *p) { (void)ctx; sent = *p; return true; }
static void event(void *ctx, enum kms_event kind, unsigned slot, uint32_t detail)
{ (void)ctx; (void)slot; if (kind == KMS_EVENT_REPLAY_DROPPED) dropped += detail; }
static kms_state_t state(unsigned key)
{ kms_state_t s = {0}; kms_set_key(&s, key, true); return s; }
static void active_sender(void)
{
    dropped = 0;
    kms_delivery_init(&sender, replay, 42, send, event, NULL);
    kms_delivery_select(&sender, 1, 7, 0); kms_delivery_tick(&sender, 0);
    kms_packet_t ack = sent; ack.type = KMS_ACK; ack.flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE;
    kms_delivery_receive(&sender, &ack, 1);
    kms_packet_t status = {.type = KMS_STATUS, .slot = 1, .session = 7, .receiver = 42,
        .generation = sender.generation, .sequence = ack.sequence, .flags = ack.flags};
    kms_batch_advertise(&status); kms_delivery_receive(&sender, &status, 1);
}
__declspec(dllexport) unsigned kms_input_batch_checks(void) { return checks; }
static int compact_codec_checks(const kms_packet_t *base)
{
    kms_packet_t packet = *base, decoded;
    uint8_t wire[KMS_MAX_WIRE_SIZE + 1];
    // Every sparse/dense boundary, including all 256 simultaneously held keys.
    for (unsigned held = 0; held <= 256; ++held) {
        memset(packet.state.keys, 0, sizeof(packet.state.keys));
        for (unsigned key = 0; key < held; ++key) kms_set_key(&packet.state, key, true);
        for (unsigned i = 0; i < 2; ++i) packet.extra[i].state = packet.state;
        size_t bytes = kms_wire_encode(&packet, wire, sizeof(wire));
        CHECK(bytes && bytes <= KMS_MAX_WIRE_SIZE && kms_wire_decode(&decoded, wire, bytes));
        CHECK(kms_state_equal(&decoded.state, &packet.state));
        CHECK(kms_state_equal(&decoded.extra[1].state, &packet.extra[1].state));
    }
    packet.batch_count = 4;
    for (unsigned i = 0; i < 4; ++i) {
        kms_state_t *held = i ? &packet.extra[i - 1].state : &packet.state;
        memset(held->keys, 0, sizeof(held->keys));
        for (unsigned key = 0; key < 27u + (unsigned)(i >= 2); ++key) kms_set_key(held, key, true);
    }
    CHECK(kms_batch_wire_size(&packet) == 250);
    CHECK(kms_wire_encode(&packet, wire, sizeof(wire)) == 250);
    CHECK(kms_wire_decode(&decoded, wire, 250) && decoded.batch_count == 4);
    CHECK(!kms_wire_encode(&packet, wire, 249) && !kms_wire_decode(&decoded, wire, 251));
    for (unsigned length = 0; length < 250; ++length) CHECK(!kms_wire_decode(&decoded, wire, length));
    wire[26] = wire[25]; CHECK(!kms_wire_decode(&decoded, wire, 250)); // Duplicate sparse usage.
    wire[26] = 1; wire[24] = 33; CHECK(!kms_wire_decode(&decoded, wire, 250));
    memset(packet.state.keys, 0xff, sizeof(packet.state.keys));
    for (unsigned i = 0; i < 3; ++i) packet.extra[i].state = packet.state;
    CHECK(kms_batch_wire_size(&packet) == 268 && !kms_wire_encode(&packet, wire, sizeof(wire)));
    return 0;
}
__declspec(dllexport) int kms_run_input_batch_tests(void)
{
    checks = 0;
    kms_packet_t status = {.type = KMS_STATUS};
    CHECK(kms_batch_limit(&status) == 1);
    kms_batch_advertise(&status); CHECK(kms_batch_limit(&status) == KMS_INPUT_BATCH_MAX);
    status.public_key[5] = 4; CHECK(kms_batch_limit(&status) == 4);
    status.type = KMS_ACK; CHECK(kms_batch_limit(&status) == 1);
    status.type = KMS_STATUS; status.public_key[4] = 1; CHECK(kms_batch_limit(&status) == 1);
    status.public_key[4] = KMS_BATCH_VERSION; status.public_key[5] = KMS_INPUT_BATCH_MAX + 1;
    CHECK(kms_batch_limit(&status) == 1);
    kms_packet_t packet = {.type = KMS_INPUT_BATCH, .slot = 1, .session = 42, .receiver = 7,
        .generation = 1, .sequence = 2, .batch_count = 3, .state = {.modifiers = 3, .buttons = 5},
        .motion = {0xffffffffu, 8, 9, 10}};
    packet.state.keys[31] = 0x80; packet.state.consumer[3] = 0x1234; packet.state.system = 7;
    packet.extra[0] = (kms_packet_input_t){.state = {.buttons = 1}, .motion = {1, 6, 9, 10}};
    packet.extra[1] = (kms_packet_input_t){.motion = {2, 6, 8, 9}};
    uint8_t wire[KMS_MAX_WIRE_SIZE + 1]; kms_packet_t decoded;
    size_t bytes = kms_wire_encode(&packet, wire, sizeof(wire));
    CHECK(bytes == 112 && bytes == kms_batch_wire_size(&packet));
    CHECK(kms_wire_decode(&decoded, wire, bytes));
    CHECK(decoded.type == KMS_INPUT_BATCH && decoded.batch_count == 3 && decoded.sequence == 2);
    CHECK(kms_state_equal(&decoded.state, &packet.state) && !memcmp(decoded.motion, packet.motion, sizeof(packet.motion)));
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(kms_state_equal(&decoded.extra[i].state, &packet.extra[i].state));
        CHECK(!memcmp(decoded.extra[i].motion, packet.extra[i].motion, sizeof(packet.motion)));
    }
    for (size_t size = 0; size < bytes; ++size) CHECK(!kms_wire_decode(&decoded, wire, size));
    CHECK(!kms_wire_decode(&decoded, wire, bytes + 1));
    CHECK(!kms_wire_encode(&packet, wire, bytes - 1));
    const unsigned fields[] = {4, 5, 6, 7, 20};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        unsigned index = fields[i]; uint8_t old = wire[index];
        wire[index] = index == 7 ? 1 : 0;
        CHECK(!kms_wire_decode(&decoded, wire, bytes)); wire[index] = old;
    }
    packet.batch_count = 2;
    CHECK(kms_wire_encode(&packet, wire, sizeof(wire)) == 83);
    packet.batch_count = 1; CHECK(!kms_wire_encode(&packet, wire, sizeof(wire)));
    packet.batch_count = KMS_INPUT_BATCH_MAX + 1; CHECK(!kms_wire_encode(&packet, wire, sizeof(wire)));
    packet.batch_count = 3;
    int failure = compact_codec_checks(&packet); if (failure) return failure;
    // The largest negotiated sparse batch still has a strict 250-byte boundary.
    // Its seven states contain 23 key usages in total: 24 + 7*29 + 23 = 250.
    kms_packet_t large = {.type = KMS_INPUT_BATCH, .slot = 1, .session = 42, .receiver = 7,
        .generation = 1, .sequence = 2, .batch_count = 7};
    for (unsigned i = 0; i < 7; ++i) {
        kms_packet_input_t *tail = i ? &large.extra[i - 1] : NULL;
        kms_state_t *held_state = tail ? &tail->state : &large.state;
        uint32_t *motion = tail ? tail->motion : large.motion;
        for (unsigned key = 0; key < 3u + (unsigned)(i >= 5); ++key)
            kms_set_key(held_state, 4 + key + i, true);
        motion[0] = i + 1;
    }
    CHECK(kms_wire_encode(&large, wire, sizeof(wire)) == 250);
    CHECK(kms_wire_decode(&decoded, wire, 250) && decoded.batch_count == 7);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(kms_state_equal(&decoded.extra[i].state, &large.extra[i].state));
        CHECK(decoded.extra[i].motion[0] == i + 2);
    }
    kms_receiver_guard_t large_guard = {.initialized = true, .active = true,
        .session = 42, .generation = 1, .sequence = 1};
    kms_sample_t large_samples[KMS_INPUT_BATCH_MAX]; unsigned large_count; bool large_apply;
    CHECK(kms_receiver_accept_many(&large_guard, &decoded, 10, large_samples, &large_count, &large_apply));
    CHECK(large_apply && large_count == 7 && large_guard.motion[0] == 7);
    for (unsigned i = 0; i < 7; ++i) {
        const kms_state_t *expected = i ? &large.extra[i - 1].state : &large.state;
        CHECK(kms_state_equal(&large_samples[i].state, expected) && large_samples[i].x == 1);
    }
    CHECK(kms_receiver_accept_many(&large_guard, &decoded, 11, large_samples, &large_count, &large_apply));
    CHECK(!large_apply && !large_count && large_guard.motion[0] == 7);
    CHECK(!kms_wire_encode(&large, wire, 249));
    for (unsigned size = 0; size < 250; ++size) CHECK(!kms_wire_decode(&decoded, wire, size));
    CHECK(!kms_wire_decode(&decoded, wire, 251));
    // Four-input receivers must never receive the larger extension.
    active_sender(); sender.batch_limit = 4;
    for (unsigned i = 0; i < 7; ++i) {
        kms_state_t held_state = state(4 + i);
        kms_delivery_input(&sender, &held_state, 1, 0, 0, 0, 2 + i);
    }
    kms_delivery_tick(&sender, 10);
    CHECK(sender.pending_count == 4 && sent.batch_count == 4 && sender.count == 7);
    // A capable receiver takes seven ordered sparse states in one receipt.
    active_sender();
    for (unsigned i = 0; i < 7; ++i) {
        kms_state_t held_state = state(4 + i);
        kms_delivery_input(&sender, &held_state, 1, 0, 0, 0, 2 + i);
    }
    kms_delivery_tick(&sender, 10);
    CHECK(sender.pending_count == 7 && sent.batch_count == 7 && sender.count == 7);
    CHECK(kms_wire_encode(&sent, wire, sizeof(wire)) == 234);
    kms_packet_t large_ack = sent; large_ack.type = KMS_ACK;
    large_ack.flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE;
    kms_delivery_receive(&sender, &large_ack, 11);
    CHECK(!sender.count && !sender.pending_count && sender.confirmed_motion[0] == 7);
    // Seven less-sparse states exceed the byte cap: send six and retain the
    // remaining two inputs, rather than truncating or exceeding the frame.
    active_sender();
    kms_state_t six_keys = {0};
    for (unsigned key = 4; key < 10; ++key) kms_set_key(&six_keys, key, true);
    for (unsigned i = 0; i < 8; ++i) {
        six_keys.buttons = i & 1;
        kms_delivery_input(&sender, &six_keys, 1, 0, 0, 0, 2 + i);
    }
    kms_delivery_tick(&sender, 10);
    CHECK(sent.batch_count == 6 && sender.pending_count == 6 && sender.count == 8);
    CHECK(kms_wire_encode(&sent, wire, sizeof(wire)) == 234);
    large_ack = sent; large_ack.type = KMS_ACK;
    large_ack.flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE;
    kms_delivery_receive(&sender, &large_ack, 11);
    CHECK(sender.count == 2 && sender.confirmed_motion[0] == 6);
    kms_delivery_tick(&sender, 12);
    CHECK(sent.batch_count == 2 && sent.motion[0] == 7 && sent.extra[0].motion[0] == 8);
    kms_receiver_guard_t guard = {.initialized = true, .active = true, .session = 42, .generation = 1, .sequence = 1};
    kms_sample_t samples[KMS_INPUT_BATCH_MAX]; unsigned count; bool apply;
    CHECK(kms_receiver_accept_many(&guard, &packet, 10, samples, &count, &apply) && apply && count == 3);
    CHECK(samples[0].x == -1 && samples[1].x == 2 && samples[2].x == 1);
    CHECK(samples[1].y == -2 && samples[2].wheel == -1 && samples[2].pan == -1);
    CHECK(kms_state_equal(&samples[0].state, &packet.state) && !samples[2].state.buttons);
    CHECK(guard.sequence == 2 && guard.motion[0] == 2);
    CHECK(kms_receiver_accept_many(&guard, &packet, 11, samples, &count, &apply) && !apply && !count);
    packet.sequence = 1; CHECK(!kms_receiver_accept_many(&guard, &packet, 12, samples, &count, &apply));
    packet.sequence = 3; ++packet.generation;
    CHECK(!kms_receiver_accept_many(&guard, &packet, 12, samples, &count, &apply));
    --packet.generation; guard.active = false;
    CHECK(!kms_receiver_accept_many(&guard, &packet, 12, samples, &count, &apply));
    guard.active = true; guard.sequence = 0xffffffffu; packet.sequence = 1;
    CHECK(kms_receiver_accept_many(&guard, &packet, 12, samples, &count, &apply) && apply && count == 3);

    // Coalescing may never change any sample already in the pending batch.
    active_sender(); CHECK(sender.batch_limit == KMS_INPUT_BATCH_MAX);
    for (unsigned i = 0; i < 3; ++i) {
        kms_state_t held = state(4 + i); kms_delivery_input(&sender, &held, 1, 0, 0, 0, 2 + i);
    }
    kms_delivery_tick(&sender, 5);
    CHECK(sent.type == KMS_INPUT_BATCH && sender.pending_count == 3 && sender.count == 3);
    kms_state_t held = state(6); kms_delivery_input(&sender, &held, 1, 0, 0, 0, 6);
    CHECK(sender.count == 4 && sender.pending.extra[1].motion[0] == 3);
    kms_packet_t ack = sent; ack.type = KMS_ACK; ack.flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE;
    kms_delivery_receive(&sender, &ack, 7);
    CHECK(sender.count == 1 && !sender.pending_count && sender.confirmed_motion[0] == 3);
    CHECK(kms_state_equal(&sender.confirmed, &held));
    kms_delivery_tick(&sender, 8); CHECK(sent.type == KMS_INPUT && sent.motion[0] == 4);

    // After a reboot, the acceptance of every in-flight sample is uncertain.
    // Do not replay any of those samples into the receiver's empty dedup state.
    active_sender();
    sender.batch_limit = 3;
    for (unsigned i = 0; i < 4; ++i) {
        kms_state_t held_state = state(4 + i); kms_delivery_input(&sender, &held_state, 1, 0, 0, 0, 2 + i);
    }
    kms_delivery_tick(&sender, 6); CHECK(sender.pending_count == 3);
    status = (kms_packet_t){.type = KMS_STATUS, .slot = 1, .session = 8, .flags = KMS_FLAG_USB_READY};
    kms_batch_advertise(&status); kms_delivery_receive(&sender, &status, 7);
    CHECK(dropped == 3 && sender.count == 1 && !sender.pending_count);
    CHECK(sender.receiver == 8 && sender.pending.type == KMS_ACTIVATE && sender.confirmed_motion[0] == 3);
    kms_delivery_stop(&sender); CHECK(sender.batch_limit == 1 && !sender.pending_count);
    // Four dense NKRO states exceed 250 bytes, so preserve the fourth unsent
    // while transmitting the first three. No queue entry is silently skipped.
    active_sender();
    memset(held.keys, 0xff, sizeof(held.keys));
    for (unsigned i = 0; i < 4; ++i) {
        held.buttons = (uint16_t)(1u << (i % 2));
        kms_delivery_input(&sender, &held, 1, 0, 0, 0, 2 + i);
    }
    kms_delivery_tick(&sender, 6);
    CHECK(sender.pending_count == 3 && sender.count == 4 && sent.batch_count == 3);
    CHECK(kms_wire_encode(&sent, wire, sizeof(wire)) == 207);
    ack = sent; ack.type = KMS_ACK; ack.flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE;
    kms_delivery_receive(&sender, &ack, 7);
    CHECK(sender.count == 1 && sender.confirmed_motion[0] == 3);
    kms_delivery_tick(&sender, 8);
    CHECK(sent.type == KMS_INPUT && sent.motion[0] == 4 && kms_state_equal(&sent.state, &held));

    active_sender();
    for (unsigned i = 0; i < 4; ++i) {
        held = state(4 + i); kms_delivery_input(&sender, &held, 1, 0, 0, 0, 2 + i);
    }
    kms_delivery_tick(&sender, 6);
    CHECK(sender.pending_count == 4 && sent.batch_count == 4);
    CHECK(kms_wire_encode(&sent, wire, sizeof(wire)) == 144);
    ack = sent; ack.type = KMS_ACK; ack.flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE;
    kms_delivery_receive(&sender, &ack, 7);
    CHECK(!sender.count && sender.confirmed_motion[0] == 4 && kms_state_equal(&sender.confirmed, &held));
    return 0;
}
