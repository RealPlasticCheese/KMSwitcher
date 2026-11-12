// Real shortcut router, sender and two receiver guards; asynchronous fake radio.
#include "kms_delivery.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
enum { PEERS = 2, RADIO_ONLINE_MS = 2, RADIO_OFFLINE_MS = 240 };
typedef struct {
    kms_receiver_guard_t guard;
    kms_state_t output, observed;
    uint32_t boot;
    unsigned activations, selections, reports, movement_reports;
    bool online, usb_ready, drop_ack;
} peer_t;
static kms_delivery_t sender;
static kms_buffered_t replay[KMS_RECOVERY_BUFFER_DEPTH];
static kms_router_t router;
static peer_t peers[PEERS];
static kms_packet_t in_flight;
static uint32_t now, completed_at, last_status;
static unsigned checks, events[PEERS][KMS_EVENT_COUNT], deferred, command_leaks;
static bool radio_busy, reject_release;
static void event(void *ctx, enum kms_event e, unsigned slot, uint32_t detail)
{ (void)ctx; (void)detail; if (slot && slot <= PEERS) ++events[slot - 1][e]; }
static bool send(void *ctx, const kms_packet_t *p)
{
    (void)ctx;
    // An offline MAC attempt occupies the radio well beyond the 80 ms app retry
    // window. It must finish before another frame can actually be submitted.
    if (radio_busy || (reject_release && p->type == KMS_RELEASE)) { ++deferred; return false; }
    in_flight = *p; radio_busy = true;
    completed_at = now + (peers[p->slot - 1].online ? RADIO_ONLINE_MS : RADIO_OFFLINE_MS);
    return true;
}
static void receive(void)
{
    if (!radio_busy || !kms_time_elapsed(now, completed_at, 0)) return;
    radio_busy = false;
    peer_t *peer = &peers[in_flight.slot - 1];
    if (!peer->online || in_flight.receiver != peer->boot ||
        (!peer->usb_ready && in_flight.type != KMS_RELEASE)) return;
    kms_sample_t sample; bool apply;
    if (!kms_receiver_accept(&peer->guard, &in_flight, now, &sample, &apply)) return;
    if (apply) {
        peer->output = sample.state; kms_state_merge(&peer->observed, &sample.state); ++peer->reports;
        if (sample.x || sample.y || sample.wheel || sample.pan) ++peer->movement_reports;
        if (in_flight.type == KMS_ACTIVATE) ++peer->activations;
        if (kms_key(&sample.state, KMS_HID_KEY_INSERT) || kms_key(&sample.state, KMS_HID_KEY_1) ||
            kms_key(&sample.state, KMS_HID_KEY_1 + 1)) ++command_leaks;
    }
    if (peer->drop_ack) return;
    kms_packet_t ack = in_flight; ack.type = KMS_ACK;
    ack.flags = (peer->usb_ready ? KMS_FLAG_USB_READY : 0) | (peer->guard.active ? KMS_FLAG_ACTIVE : 0);
    kms_delivery_receive(&sender, &ack, now);
}
static void status(void)
{
    for (unsigned i = 0; i < PEERS; ++i) if (peers[i].online) {
        peer_t *peer = &peers[i];
        kms_packet_t p = {.type = KMS_STATUS, .slot = (uint8_t)(i + 1), .session = peer->boot,
            .receiver = peer->guard.session, .generation = peer->guard.generation, .sequence = peer->guard.sequence,
            .flags = (peer->usb_ready ? KMS_FLAG_USB_READY : 0) | (peer->guard.active ? KMS_FLAG_ACTIVE : 0)};
        kms_delivery_receive(&sender, &p, now);
    }
}
static void advance(uint32_t duration)
{
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += KMS_MAIN_POLL_MS) {
        now += KMS_MAIN_POLL_MS;
        for (unsigned i = 0; i < PEERS; ++i)
            if (kms_receiver_expire(&peers[i].guard, now)) memset(&peers[i].output, 0, sizeof(peers[i].output));
        receive();
        if (kms_time_elapsed(now, last_status, KMS_STATUS_MS)) { status(); last_status = now; }
        kms_delivery_tick(&sender, now);
        // Match the controller's fault/held-input handling before USB reports.
        if (sender.block) { sender.block = false; kms_router_block_output(&router); }
        kms_router_tick(&router, now);
    }
}
static bool select_slot(void *ctx, unsigned slot)
{
    (void)ctx; ++peers[slot - 1].selections;
    if (!peers[slot - 1].online || !peers[slot - 1].usb_ready) return false;
    kms_delivery_select(&sender, slot, peers[slot - 1].boot, now); return true;
}
static void emit(void *ctx, const kms_state_t *state, int32_t x, int32_t y, int32_t wheel, int32_t pan)
{ (void)ctx; kms_delivery_input(&sender, state, x, y, wheel, pan, now); }
static void key(unsigned input, unsigned usage)
{
    kms_sample_t sample = {0}; if (usage) kms_set_key(&sample.state, usage, true);
    kms_router_input(&router, input, &sample, now); advance(40);
}
static void shortcut(unsigned input, unsigned slot)
{
    key(input, KMS_HID_KEY_INSERT); key(input, 0);
    key(input, KMS_HID_KEY_INSERT); key(input, 0);
    key(input, KMS_HID_KEY_1 + slot - 1); key(input, 0);
}
static void setup(uint32_t start)
{
    memset(peers, 0, sizeof(peers)); memset(events, 0, sizeof(events));
    for (unsigned i = 0; i < PEERS; ++i) { peers[i].boot = 7 + i; peers[i].online = peers[i].usb_ready = true; }
    deferred = command_leaks = 0; radio_busy = reject_release = false; now = last_status = start;
    kms_delivery_init(&sender, replay, 42, send, event, NULL);
    kms_router_init(&router, emit, select_slot, NULL, NULL);
    (void)kms_router_set_trigger(&router, KMS_TRIGGER_INSERT);
    kms_delivery_select(&sender, 1, peers[0].boot, now); advance(20);
}
static int disconnected_switch(uint32_t wait, unsigned input, uint32_t start)
{
    setup(start); shortcut(input, 1); advance(20);
    CHECK(sender.slot == 1 && peers[0].guard.active && !sender.recovering);
    peers[0].online = false; advance(wait);
    shortcut(input, 2); advance(1000);
    CHECK(peers[1].selections == 1 && sender.slot == 2);
    CHECK(events[1][KMS_EVENT_ACK_TIMEOUT] == 0);
    CHECK(peers[1].guard.active && peers[1].activations == 1 && !sender.recovering);
    CHECK(!sender.suppressed && !sender.count && deferred && !command_leaks);
    key(input, 4); CHECK(kms_key(&peers[1].output, 4));
    key(input, 0); CHECK(!kms_key(&peers[1].output, 4));
    CHECK(peers[1].selections == 1);
    return 0;
}
static int expiry_during_shortcut(unsigned boundary, unsigned input, uint32_t start, bool usb_suspend)
{
    setup(start);
    if (usb_suspend) { peers[0].usb_ready = false; status(); }
    else peers[0].online = false;
    key(input, 4); key(input, 0);
    uint32_t expiry = sender.buffer[sender.head].at + KMS_RECOVERY_BUFFER_MS;
    // Put the expiry halfway through each of the four Insert down/up stages.
    advance(expiry - now - boundary * 40 - 20);
    shortcut(input, 2); advance(1000);
    CHECK(sender.slot == 2 && peers[1].selections == 1);
    CHECK(peers[1].guard.active && !sender.recovering && !sender.count);
    CHECK(!command_leaks && !kms_key(&peers[1].observed, 4));
    key(input, 5); CHECK(kms_key(&peers[1].output, 5)); key(input, 0);
    return 0;
}
static int transition_during_shortcut(unsigned boundary, unsigned cause)
{
    setup(1000); peers[0].online = false;
    const unsigned edges[] = {KMS_HID_KEY_INSERT, 0, KMS_HID_KEY_INSERT, 0};
    for (unsigned n = 0; n <= 4; ++n) {
        if (n == boundary) {
            if (cause == 0) {
                for (unsigned k = 0; k <= KMS_RECOVERY_BUFFER_DEPTH; ++k) {
                    kms_state_t state = {0}; if (k % 2) kms_set_key(&state, 4, true);
                    kms_delivery_input(&sender, &state, 0, 0, 0, 0, now);
                }
            } else if (cause == 1) kms_delivery_abandon(&sender, KMS_EVENT_RX_OVERFLOW, now);
            else {
                // Recovery after suppression requests another held-output block.
                kms_delivery_abandon(&sender, KMS_EVENT_ACK_TIMEOUT, now); advance(4);
                peers[0].online = true;
                if (cause == 3) { ++peers[0].boot; memset(&peers[0].guard, 0, sizeof(peers[0].guard)); }
                status();
            }
            advance(20);
        }
        if (n < 4) key(0, edges[n]);
    }
    key(0, KMS_HID_KEY_1 + 1); key(0, 0); advance(1000);
    CHECK(sender.slot == 2 && peers[1].selections == 1 && peers[1].guard.active);
    CHECK(!sender.recovering && !sender.suppressed && !sender.count && !command_leaks);
    CHECK(!kms_key(&peers[1].observed, 4));
    return 0;
}
static int obsolete_block_after_switch(void)
{
    setup(1000); kms_delivery_abandon(&sender, KMS_EVENT_ACK_TIMEOUT, now);
    CHECK(sender.block);
    // A selection within the same USB batch already blocks the old held state.
    // Its old pending block must not suppress fresh input on the new route.
    kms_delivery_select(&sender, 2, peers[1].boot, now);
    CHECK(!sender.block);
    key(0, 4); advance(500); CHECK(kms_key(&peers[1].output, 4)); key(0, 0);
    setup(1000); kms_delivery_abandon(&sender, KMS_EVENT_ACK_TIMEOUT, now);
    kms_delivery_stop(&sender); CHECK(!sender.block && !sender.slot);
    return 0;
}
static int delayed_and_repeated_handover(void)
{
    // A lost old-route ACK may arrive during the new activation. It cannot
    // confirm that activation, roll back selection or restore the old queue.
    setup(1000); peers[0].drop_ack = true; key(0, 4);
    kms_packet_t late = sender.pending; late.type = KMS_ACK;
    late.flags = KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE;
    key(0, 0); shortcut(0, 2);
    uint32_t generation = sender.generation;
    kms_delivery_receive(&sender, &late, now); advance(1000);
    CHECK(sender.slot == 2 && sender.generation == generation && !sender.recovering);
    CHECK(peers[1].guard.active && !kms_key(&peers[1].observed, 4) && !command_leaks);
    CHECK(events[1][KMS_EVENT_ACK_TIMEOUT] == 0);
    // Status can prove a destination received activation despite all lost ACKs.
    setup(1000); peers[1].drop_ack = true; shortcut(0, 2); advance(1200); status();
    CHECK(sender.slot == 2 && peers[1].guard.active && !sender.recovering);
    CHECK(peers[1].activations == 1 && !sender.count && !command_leaks);
    // A radio completion from the failed PC can still be busy while two local
    // commands change the selected destination, including back to that PC.
    for (unsigned same = 0; same < 2; ++same) {
        setup(1000); peers[0].online = false; key(0, 4); key(0, 0);
        shortcut(0, 2);
        if (same) shortcut(0, 2);
        else { peers[0].online = true; shortcut(0, 1); }
        advance(1200);
        unsigned target = same ? 1 : 0;
        CHECK(sender.slot == target + 1 && peers[target].guard.active && !sender.recovering);
        CHECK(!sender.count && !sender.block && !command_leaks);
        key(0, 5); CHECK(kms_key(&peers[target].output, 5)); key(0, 0);
    }
    // A genuinely failed destination stays selected for automatic recovery;
    // the user never has to repeat a successfully recognized shortcut.
    setup(1000); shortcut(0, 2); peers[1].usb_ready = false; status(); advance(6000);
    CHECK(sender.slot == 2 && sender.suppressed && peers[1].selections == 1);
    peers[1].usb_ready = true; status(); advance(1200);
    CHECK(sender.slot == 2 && peers[1].guard.active && !sender.recovering && !sender.suppressed);
    key(0, 5); CHECK(kms_key(&peers[1].output, 5)); key(0, 0);
    // If the old PC cannot receive its release, its own lease still clears all
    // held output. Held keys on another keyboard never migrate to the new PC.
    setup(1000); key(1, 4); peers[0].online = false;
    CHECK(kms_key(&peers[0].output, 4));
    kms_router_block_output(&router); // Delivery has discarded its held state.
    key(1, 0); shortcut(0, 2); advance(KMS_LEASE_MS + 1000);
    CHECK(!kms_key(&peers[0].output, 4) && !kms_key(&peers[1].observed, 4));
    CHECK(peers[1].guard.active && sender.slot == 2 && !command_leaks);
    return 0;
}
__declspec(dllexport) int kms_run_switching_tests(void)
{
    checks = 0;
    for (unsigned boundary = 0; boundary < 4; ++boundary)
        for (unsigned input = 0; input < 2; ++input)
            for (unsigned suspend = 0; suspend < 2; ++suspend)
                for (unsigned wrap = 0; wrap < 2; ++wrap) {
                    int failure = expiry_during_shortcut(boundary, input, wrap ? 0xffffff00u : 1000, suspend != 0);
                    if (failure) return failure;
                }
    for (unsigned boundary = 0; boundary < 5; ++boundary)
        for (unsigned cause = 0; cause < 4; ++cause) {
            int failure = transition_during_shortcut(boundary, cause); if (failure) return failure;
        }
    int obsolete = obsolete_block_after_switch(); if (obsolete) return obsolete;
    int delayed = delayed_and_repeated_handover(); if (delayed) return delayed;
    // No pause after the second Insert: overlap, then release in either order.
    for (unsigned release_trigger = 0; release_trigger < 2; ++release_trigger) {
        setup(1000);
        key(0, KMS_HID_KEY_INSERT); key(0, 0); key(0, KMS_HID_KEY_INSERT);
        kms_sample_t fast = {0};
        kms_set_key(&fast.state, KMS_HID_KEY_INSERT, true);
        kms_set_key(&fast.state, KMS_HID_KEY_1 + 1, true);
        kms_router_input(&router, 0, &fast, now);
        CHECK(sender.slot == 2 && peers[1].selections == 1);
        key(0, release_trigger ? KMS_HID_KEY_1 + 1 : KMS_HID_KEY_INSERT);
        key(0, 0); advance(1000);
        CHECK(peers[1].guard.active && !sender.count && !command_leaks);
        key(0, 4); CHECK(kms_key(&peers[1].output, 4)); key(0, 0);
    }
    const uint32_t waits[] = {0, 5000, 10000};
    for (unsigned i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i)
        for (unsigned input = 0; input < 2; ++input) {
            int failure = disconnected_switch(waits[i], input, 1000);
            if (failure) return failure;
        }
    int failure = disconnected_switch(5000, 0, 0xffffff00u);
    if (failure) return failure;
    // Input queued for the disconnected destination must never reach the new PC.
    setup(1000); peers[0].online = false;
    key(0, 4); key(0, 0);
    kms_sample_t movement = {.x = 11, .wheel = 1};
    kms_router_input(&router, 0, &movement, now); advance(400);
    CHECK(sender.count && sender.recovering);
    shortcut(0, 2); advance(1000);
    CHECK(sender.slot == 2 && peers[1].guard.active && !sender.count && !command_leaks);
    CHECK(!kms_key(&peers[1].observed, 4) && !peers[1].movement_reports);
    CHECK(events[1][KMS_EVENT_ACK_TIMEOUT] == 0);
    // Driver rejection of the old release must not hold up a new destination.
    setup(1000); peers[0].online = false; reject_release = true;
    advance(10000); shortcut(0, 2); advance(1000);
    CHECK(sender.slot == 2 && peers[1].guard.active && peers[1].selections == 1);
    CHECK(events[1][KMS_EVENT_ACK_TIMEOUT] == 0 && !sender.releasing);
    // An actually unavailable destination still keeps the old selection.
    setup(1000); peers[0].online = peers[1].online = false;
    advance(10000); shortcut(0, 2);
    CHECK(sender.slot == 1 && peers[1].selections == 1 && !peers[1].activations);
    return 0;
}
__declspec(dllexport) int kms_switching_test_count(void) { return checks; }
