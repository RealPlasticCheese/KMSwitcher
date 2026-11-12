// Accepted input must survive a busy transmitter without waiting for retransmit.
#include "kms_ack.h"
#include "kms_delivery.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, sends;
static bool busy;
static kms_packet_t sent_packet;
static bool transmit(void *ctx, const kms_packet_t *packet)
{
    (void)ctx;
    if (busy) return false;
    sent_packet = *packet; ++sends; return true;
}
static kms_delivery_t sender;
static kms_buffered_t replay[KMS_RECOVERY_BUFFER_DEPTH];
static kms_receiver_guard_t receiver;
static kms_ack_t response;
static uint32_t now;
static unsigned moved, faults;
static bool acknowledge(void *ctx, const kms_packet_t *packet)
{
    (void)ctx;
    if (now % 5 < 2) return false; // Status occupies the first two milliseconds.
    kms_delivery_receive(&sender, packet, now); return true;
}
static bool receive_input(void *ctx, const kms_packet_t *packet)
{
    (void)ctx;
    kms_sample_t sample; bool apply;
    if (!kms_receiver_accept(&receiver, packet, now, &sample, &apply)) return true;
    if (apply) moved += (unsigned)sample.x;
    kms_ack_prepare(&response, packet);
    response.packet.flags = KMS_FLAG_USB_READY | (receiver.active ? KMS_FLAG_ACTIVE : 0);
    kms_ack_flush(&response, acknowledge, NULL);
    return true;
}
static void delivery_fault(void *ctx, enum kms_event event, unsigned slot, uint32_t detail)
{ (void)ctx; (void)event; (void)slot; (void)detail; ++faults; }
__declspec(dllexport) int kms_run_ack_tests(void)
{
    checks = sends = 0;
    kms_ack_t ack = {0};
    kms_packet_t input = {.type = KMS_INPUT, .slot = 6, .session = 42, .receiver = 7,
                         .generation = 9, .sequence = 15};
    kms_ack_prepare(&ack, &input);
    busy = true; kms_ack_flush(&ack, transmit, NULL);
    CHECK(ack.pending && sends == 0);
    // The next free transmitter opportunity sends the original receipt.
    busy = false; kms_ack_flush(&ack, transmit, NULL);
    CHECK(!ack.pending && sends == 1);
    CHECK(sent_packet.type == KMS_ACK && sent_packet.sequence == 15);
    CHECK(sent_packet.session == 42 && sent_packet.receiver == 7 && sent_packet.generation == 9 && sent_packet.slot == 6);
    kms_ack_flush(&ack, transmit, NULL); CHECK(sends == 1);
    // Newer accepted input replaces an older queued receipt. The sender cannot
    // advance to it without already receiving the preceding receipt/status.
    kms_ack_prepare(&ack, &input); ++input.sequence; kms_ack_prepare(&ack, &input);
    kms_ack_flush(&ack, transmit, NULL); CHECK(sends == 2 && sent_packet.sequence == 16);
    // Flags are refreshed by the owner after lease expiry, before transmitting.
    kms_ack_prepare(&ack, &input); ack.packet.flags = KMS_FLAG_USB_READY;
    kms_ack_flush(&ack, transmit, NULL); CHECK(!(sent_packet.flags & KMS_FLAG_ACTIVE));
    // Receipt storage discards the decoded batch tail. The fixed wire reply
    // still echoes the original payload and includes current USB/LED status.
    input.type = KMS_INPUT_BATCH; input.batch_count = 2;
    input.state.modifiers = 8; input.motion[1] = 22; input.extra[0].motion[1] = 99;
    kms_ack_prepare(&ack, &input);
    ack.packet.flags = KMS_FLAG_ACTIVE;
    ack.packet.keyboard_leds = (kms_led_state_t){.valid = true, .leds = KMS_LOCK_CAPS, .sequence = 7};
    kms_ack_flush(&ack, transmit, NULL);
    input.type = KMS_ACK; input.flags = KMS_FLAG_ACTIVE; input.keyboard_leds = ack.packet.keyboard_leds;
    uint8_t old_wire[KMS_WIRE_SIZE], new_wire[KMS_WIRE_SIZE];
    CHECK(kms_wire_encode(&input, old_wire, sizeof(old_wire)) == KMS_WIRE_SIZE);
    CHECK(kms_wire_encode(&sent_packet, new_wire, sizeof(new_wire)) == KMS_WIRE_SIZE);
    CHECK(!memcmp(old_wire, new_wire, sizeof(old_wire)) && !sent_packet.batch_count);
    // Exercise the production delivery queue, receiver deduplication and ACK
    // owner at 200 samples/s while every initial receipt hits a busy radio.
    memset(&receiver, 0, sizeof(receiver)); memset(&response, 0, sizeof(response));
    now = moved = faults = 0;
    kms_delivery_init(&sender, replay, 42, receive_input, delivery_fault, NULL);
    kms_delivery_select(&sender, 6, 7, now);
    unsigned generated = 0, peak = 0;
    for (now = 0; now < 2020; ++now) {
        kms_ack_flush(&response, acknowledge, NULL);
        if (now >= 10 && now < 2010 && now % 5 == 0) {
            kms_state_t state = {0}; kms_set_key(&state, generated % 2 ? 4 : 5, true);
            kms_delivery_input(&sender, &state, 1, 0, 0, 0, now); ++generated;
        }
        kms_delivery_tick(&sender, now);
        if (sender.count > peak) peak = sender.count;
    }
    CHECK(generated == 400 && moved == 400);
    CHECK(!sender.count && !sender.has_pending && !response.pending);
    CHECK(peak <= 1 && faults == 0);
    return 0;
}
__declspec(dllexport) unsigned kms_ack_test_count(void) { return checks; }
