#include "kms_text.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, sends;
static bool busy;
static kms_packet_t sent;
static bool send(void *ctx, const kms_packet_t *p)
{ (void)ctx; if (busy) return false; ++sends; sent = *p; return true; }
static void packet(kms_packet_t *p, unsigned sequence, unsigned operation, const char *data, size_t length)
{
    *p = (kms_packet_t){.slot = 2, .session = 11, .receiver = 22, .generation = 33, .sequence = sequence};
    (void)kms_text_packet(p, operation, data, length);
}
static kms_receiver_guard_t route(void)
{ return (kms_receiver_guard_t){.initialized = true, .active = true, .session = 11, .generation = 33}; }
static int partial_lengths(const char *data)
{
    // Every partial length, including exactly one character and a full batch.
    for (unsigned length = 1; length <= KMS_TEXT_BYTES; ++length) {
        kms_packet_t p, decoded, ack; packet(&p, 1, KMS_TEXT_DATA, data, length);
        uint8_t wire[KMS_WIRE_SIZE];
        CHECK(kms_text_valid(&p));
        CHECK(kms_wire_encode(&p, wire, sizeof(wire)) == sizeof(wire));
        CHECK(kms_wire_decode(&decoded, wire, sizeof(wire)) && kms_text_valid(&decoded));
        kms_text_rx_t rx = {0}; kms_receiver_guard_t guard = route(); bool cancel;
        CHECK(!kms_text_rx_receive(&rx, &decoded, &guard, 100, &cancel) && !cancel && rx.active);
        kms_text_tx_t tx = {0}; kms_text_tx_start(&tx, &p, 100); kms_text_tx_tick(&tx, 100, send, NULL);
        for (unsigned n = 0; n < length; ++n) {
            kms_sample_t report;
            CHECK(kms_text_rx_next(&rx, false, &report));
            CHECK(kms_key(&report.state, 4 + (unsigned)(data[n] - 'a')) && !report.state.modifiers);
            CHECK(rx.offset == n && !rx.complete);
            kms_text_rx_delivered(&rx); // Completed key-down, still no batch ACK.
            CHECK(!rx.complete && rx.active);
            CHECK(!kms_text_rx_receive(&rx, &p, &guard, 101 + n, &cancel));
            CHECK(kms_text_rx_next(&rx, false, &report));
            kms_state_t zero = {0}; CHECK(kms_state_equal(&report.state, &zero));
            CHECK(!rx.complete);
            kms_text_rx_delivered(&rx);
        }
        CHECK(rx.complete && !rx.active && rx.offset == length);
        kms_text_ack(&rx.packet, &ack.single);
        // Lost ACK: retry the whole frame; no characters are generated again.
        kms_text_tx_tick(&tx, 200, send, NULL); CHECK(tx.pending);
        CHECK(kms_text_rx_receive(&rx, &sent, &guard, 200, &cancel));
        kms_sample_t report; CHECK(!kms_text_rx_next(&rx, false, &report));
        CHECK(kms_text_tx_receive(&tx, &ack) && !tx.pending);
        CHECK(!kms_text_tx_receive(&tx, &ack));
        // Caps Lock conversion is evaluated at the receiver for each press.
        packet(&p, 2, KMS_TEXT_DATA, "aA!!", 4);
        CHECK(!kms_text_rx_receive(&rx, &p, &guard, 210, &cancel));
        CHECK(kms_text_rx_next(&rx, true, &report) && report.state.modifiers == 2);
        kms_text_rx_delivered(&rx); CHECK(kms_text_rx_next(&rx, true, &report)); kms_text_rx_delivered(&rx);
        CHECK(kms_text_rx_next(&rx, true, &report) && !report.state.modifiers);
    }
    return 0;
}
__declspec(dllexport) unsigned kms_text_checks(void) { return checks; }
__declspec(dllexport) int kms_run_text_tests(void)
{
    char data[KMS_TEXT_BYTES + 1];
    for (unsigned n = 0; n < sizeof(data); ++n) data[n] = (char)('a' + n % 26);
    int failure = partial_lengths(data);
    if (failure) return failure;
    kms_packet_t p, ack; packet(&p, 1, KMS_TEXT_QUERY, NULL, 0);
    kms_text_tx_t tx = {0};
    kms_text_tx_start(&tx, &p, 0xfffffff0u); busy = true;
    kms_text_tx_tick(&tx, 0xfffffff0u, send, NULL); CHECK(!tx.submitted && tx.pending);
    kms_text_ack(&p.single, &ack.single); CHECK(!kms_text_tx_receive(&tx, &ack));
    kms_text_tx_tick(&tx, 0xfffffff0u + KMS_TEXT_PROBE_MS, send, NULL);
    CHECK(!tx.pending && !tx.supported && !tx.failed); // Old receiver fallback, including clock wrap.
    busy = false; kms_text_tx_start(&tx, &p, 200); kms_text_tx_tick(&tx, 200, send, NULL);
    kms_text_rx_t rx = {0}; kms_receiver_guard_t guard = route(); bool cancel;
    CHECK(kms_text_rx_receive(&rx, &p, &guard, 201, &cancel));
    CHECK(kms_text_tx_receive(&tx, &ack) && tx.supported);
    packet(&p, 2, KMS_TEXT_DATA, data, KMS_TEXT_BYTES);
    CHECK(!kms_text_rx_receive(&rx, &p, &guard, 202, &cancel) && rx.active);
    kms_packet_t changed = p; ++changed.generation;
    CHECK(!kms_text_rx_receive(&rx, &changed, &guard, 203, &cancel) && rx.offset == 0);
    changed = p; ++changed.session;
    CHECK(!kms_text_rx_receive(&rx, &changed, &guard, 203, &cancel));
    changed = p; changed.state.keys[0] ^= 1;
    CHECK(!kms_text_rx_receive(&rx, &changed, &guard, 203, &cancel));
    packet(&changed, 3, KMS_TEXT_DATA, "z", 1);
    CHECK(!kms_text_rx_receive(&rx, &changed, &guard, 203, &cancel) && rx.packet.sequence == 2);
    // Cancel while a synthetic key is held; stale retries cannot restart it.
    kms_sample_t report; CHECK(kms_text_rx_next(&rx, false, &report)); kms_text_rx_delivered(&rx);
    packet(&changed, 3, KMS_TEXT_CANCEL, NULL, 0);
    CHECK(kms_text_rx_receive(&rx, &changed, &guard, 204, &cancel) && cancel && !rx.active);
    CHECK(!kms_text_rx_receive(&rx, &p, &guard, 205, &cancel) && !cancel);
    CHECK(!kms_text_rx_next(&rx, false, &report));
    CHECK(kms_text_rx_receive(&rx, &changed, &guard, 206, &cancel) && !cancel);
    // A stale ACK cannot complete a newer data/cancellation packet.
    kms_text_tx_start(&tx, &changed, 210); kms_text_tx_tick(&tx, 210, send, NULL);
    kms_text_ack(&p.single, &ack.single); CHECK(!kms_text_tx_receive(&tx, &ack));
    kms_text_ack(&changed.single, &ack.single); ++ack.receiver; CHECK(!kms_text_tx_receive(&tx, &ack));
    --ack.receiver; CHECK(kms_text_tx_receive(&tx, &ack));
    kms_text_tx_start(&tx, &p, 220); kms_text_tx_tick(&tx, 2220, send, NULL);
    CHECK(tx.failed && !tx.pending);
    guard.active = false;
    CHECK(!kms_text_rx_receive(&rx, &p, &guard, 2230, &cancel));
    packet(&p, 1, KMS_TEXT_DATA, data, 1);
    CHECK(!kms_text_packet(&p, KMS_TEXT_DATA, data, 0));
    CHECK(!kms_text_packet(&p, KMS_TEXT_DATA, data, KMS_TEXT_BYTES + 1));
    CHECK(!kms_text_packet(&p, KMS_TEXT_QUERY, data, 1));
    p.motion[0] = KMS_TEXT_BYTES + 1; CHECK(!kms_text_valid(&p));
    // Repeated letters still get individual key-up reports; CR-only batches
    // complete without inventing keystrokes or leaving the sender waiting.
    rx = (kms_text_rx_t){0}; guard = route();
    packet(&p, 0xffffffffu, KMS_TEXT_DATA, "aa", 2);
    CHECK(!kms_text_rx_receive(&rx, &p, &guard, 2300, &cancel));
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(kms_text_rx_next(&rx, false, &report) && kms_key(&report.state, 4));
        kms_text_rx_delivered(&rx);
        CHECK(kms_text_rx_next(&rx, false, &report) && !kms_key(&report.state, 4));
        kms_text_rx_delivered(&rx);
    }
    CHECK(rx.complete);
    packet(&p, 1, KMS_TEXT_DATA, "\r", 1);
    CHECK(!kms_text_rx_receive(&rx, &p, &guard, 2301, &cancel));
    CHECK(!kms_text_rx_next(&rx, false, &report) && rx.complete);
    return 0;
}
