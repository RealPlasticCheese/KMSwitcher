#include "kms_stream_radio.h"
#include "esp_now.h"
#include <string.h>

static unsigned checks, rate_used, deliveries, available = 4, sends;
static uint8_t storage[4096], captured[KMS_STREAM_MAX_WIRE];
static size_t captured_size, allocated;
static bool accept_send = true;
static uint32_t peer_boot = 99;
void *kms_memory_calloc(size_t count, size_t size)
{ size_t bytes = (count * size + 7) & ~(size_t)7;
    if (allocated + bytes > sizeof(storage)) return NULL;
    void *result = storage + allocated; allocated += bytes; memset(result, 0, bytes); return result; }
int esp_now_set_peer_rate_config(const uint8_t *mac, esp_now_rate_config_t *rate)
{ (void)mac; rate_used = rate->rate; return ESP_OK; }
static unsigned receive_space(void) { return available; }
static void received(const kms_management_frame_t *f, const uint8_t *mac, int8_t rssi)
{ (void)f; (void)mac; (void)rssi; ++deliveries; }
static bool peer(unsigned slot, uint8_t *mac, uint32_t *boot)
{ memset(mac, 2, 6); *boot = peer_boot; return slot == 1; }
static bool send(const uint8_t *mac, const uint8_t *wire, size_t size)
{
    ++sends; captured_size = size; memcpy(captured, wire, size);
    kms_stream_radio_rate(mac, wire, size); return accept_send;
}
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) unsigned kms_stream_checks(void) { return checks; }
__declspec(dllexport) int kms_run_stream_tests(void)
{
    kms_stream_slot_t slots[6]; kms_stream_pool_t pool;
    kms_stream_pool_init(&pool, slots, 6);
    kms_management_frame_t f = {.op = KMS_M_OTA_WRITE, .slot = 1, .id = 1, .sender = 42, .target = 99, .length = 192};
    unsigned indices[4];
    for (unsigned i = 0; i < 6; ++i) { f.id = i + 1; CHECK(kms_stream_submit(&pool, &f, 2)); }
    CHECK(!kms_stream_submit(&pool, &f, 2)); CHECK(kms_stream_free(&pool) == 0);
    CHECK(kms_stream_peek(&pool, true, indices, 4) == 4);
    CHECK(slots[indices[0]].frame.id == 1 && slots[indices[3]].frame.id == 4);
    CHECK(kms_stream_free(&pool) == 0); // Peeking cannot lose ownership on driver rejection.
    kms_stream_release(&pool, indices, 4); CHECK(kms_stream_free(&pool) == 4);
    kms_stream_batch_t batch = {.sender = 42, .target = 99, .slot = 1, .sequence = 1, .count = 4, .profile = 2, .credit = 4}, decoded;
    for (unsigned i = 0; i < 4; ++i) { batch.frames[i] = f; batch.frames[i].id = i + 1; }
    uint8_t wire[KMS_STREAM_MAX_WIRE]; size_t size = kms_stream_encode(&batch, wire, sizeof(wire));
    CHECK(size == 940); CHECK(kms_stream_decode(&decoded, wire, size)); CHECK(decoded.frames[3].id == 4);
    for (unsigned i = 0; i < size; ++i) { wire[i] ^= 1; CHECK(!kms_stream_decode(&decoded, wire, size)); wire[i] ^= 1; }
    for (unsigned i = 0; i < size; ++i) CHECK(!kms_stream_decode(&decoded, wire, i));
    batch.frames[3].slot = 2; CHECK(!kms_stream_encode(&batch, wire, sizeof(wire))); batch.frames[3].slot = 1;
    batch.count = 0; CHECK(!kms_stream_encode(&batch, wire, sizeof(wire)));
    batch.flags = KMS_STREAM_HELLO; CHECK(kms_stream_encode(&batch, wire, sizeof(wire)) == 28);
    kms_stream_cursor_t cursor; CHECK(kms_stream_cursor_init(&cursor, 401, 1, 188));
    CHECK(!kms_stream_cursor_commit(&cursor, 188)); CHECK(kms_stream_cursor_commit(&cursor, 189));
    CHECK(kms_stream_cursor_commit(&cursor, 377)); CHECK(kms_stream_cursor_next(&cursor) == 24);
    CHECK(!kms_stream_cursor_init(&cursor, 1, 2, 188));

    kms_stream_runtime_init(); kms_stream_radio_init(42, receive_space, received);
    CHECK(kms_stream_local_status(1, 0) == (KMS_STREAM_STATUS_TAG | (4u << 8) | 255u));
    CHECK(kms_stream_local_status(0, 100) == (KMS_STREAM_STATUS_TAG | (4u << 16) | (4u << 8) | 255u));
    CHECK(kms_stream_set_profile(2)); CHECK(!kms_stream_set_profile(3));
    for (unsigned i = 0; i < 4; ++i) { f.id = i + 1; CHECK(kms_stream_enqueue(&f)); }
    CHECK(!kms_stream_enqueue(&f));
    uint8_t status[32]; CHECK(kms_stream_usb_get(status, sizeof(status)) == 32); CHECK(status[5] == 0 && status[9] == 2);
    kms_stream_radio_tick(false, 100, send, peer); CHECK(sends == 0); // Pending input wins.
    kms_stream_radio_tick(true, 100, send, peer); CHECK(captured_size == 28 && kms_stream_pending() == 4);
    kms_stream_radio_tick(true, 110, send, peer); CHECK(captured_size == 226 && kms_stream_pending() == 3); // Legacy fallback.
    kms_management_frame_t hello = {.op = KMS_M_STREAM_STATUS, .slot = 1, .sender = 99, .target = 42, .length = 4};
    memcpy(hello.data, "KB", 2); hello.data[2] = KMS_STREAM_HELLO_REPLY;
    CHECK(kms_stream_radio_control(&hello));
    accept_send = false; kms_stream_radio_tick(true, 120, send, peer); CHECK(kms_stream_pending() == 3);
    accept_send = true; kms_stream_radio_tick(true, 122, send, peer);
    CHECK(kms_stream_pending() == 0 && captured_size > 250 && rate_used == WIFI_PHY_RATE_12M);
    CHECK(kms_stream_decode(&decoded, captured, captured_size));
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(kms_stream_enqueue(&f)); kms_stream_radio_tick(true, 130 + i * 10, send, peer);
        kms_stream_radio_complete(false, 1);
    }
    CHECK(kms_stream_radio_status(status, sizeof(status)) == 32 && status[16] == 1);
    CHECK((kms_stream_local_status(1, 0) & 255u) == 1);
    kms_management_frame_t control = f; control.op = KMS_M_IDENTITY; control.length = 0;
    CHECK(kms_stream_enqueue_profile(&control, 0)); kms_stream_radio_tick(true, 170, send, peer);
    CHECK(kms_stream_radio_status(status, sizeof(status)) == 32 && status[16] == 1);
    CHECK((kms_stream_local_status(1, 0) & 255u) == 1);
    uint8_t mac[6] = {2}; kms_stream_radio_rate(mac, (const uint8_t *)"KMSW", 4);
    CHECK(rate_used == WIFI_PHY_RATE_1M_L); // Bulk settings never change input rate.
    CHECK(kms_stream_enqueue(&f)); peer_boot = 100;
    kms_stream_radio_tick(true, 200, send, peer); CHECK(kms_stream_pending() == 0);
    // Clipboard advertisements must leave the radio free for an input ACK too.
    // They are control cells, so the bulk-only admission check used to miss them.
    peer_boot = 99;
    kms_management_frame_t advert = f; advert.op = KMS_M_CLIPBOARD; advert.data[4] = 1;
    CHECK(kms_stream_enqueue(&advert));
    unsigned before = sends;
    kms_stream_radio_tick(false, 220, send, peer);
    CHECK(sends == before && kms_stream_pending() == 1);
    kms_stream_radio_tick(true, 230, send, peer);
    CHECK(sends == before + 1 && kms_stream_pending() == 0);
    // Capability replies are also background traffic; defer without losing them.
    hello.data[2] = KMS_STREAM_HELLO;
    CHECK(kms_stream_radio_control(&hello));
    before = sends;
    kms_stream_radio_tick(false, 240, send, peer); CHECK(sends == before);
    kms_stream_radio_tick(true, 250, send, peer); CHECK(sends == before + 1);
    // A whole batch is rejected under RX pressure, with no partial deliveries.
    decoded.sender = 99; decoded.target = 42;
    for (unsigned i = 0; i < decoded.count; ++i) { decoded.frames[i].sender = 99; decoded.frames[i].target = 42; }
    size = kms_stream_encode(&decoded, wire, sizeof(wire)); CHECK(size > 0);
    available = 1; kms_stream_radio_receive(wire, size, mac, -40); CHECK(deliveries == 0);
    available = 4; kms_stream_radio_receive(wire, size, mac, -40); CHECK(deliveries == decoded.count);
    return 0;
}
