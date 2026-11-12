#include "kms_management.h"
#include "kms_slave_history.h"
#include "kms_settings.h"
#include <string.h>

#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks;
static uint8_t flash[512], expected_image[256], hash[32], running_hash[32];
static unsigned begin_calls, writes, activations, aborts;
static bool write_failure, corruption, activate_failure;
static bool flash_begin(void *ctx, uint32_t size)
{ (void)ctx; ++begin_calls; memset(flash, 0xff, sizeof(flash)); return size <= sizeof(flash); }
static bool flash_write(void *ctx, uint32_t offset, const uint8_t *bytes, size_t size)
{
    (void)ctx; ++writes;
    if (write_failure) return false;
    memcpy(flash + offset, bytes, size); return true;
}
static bool flash_verify(void *ctx, uint32_t size, const uint8_t *digest)
{ (void)ctx; return !corruption && size == sizeof(expected_image) &&
    !memcmp(flash, expected_image, size) && !memcmp(digest, hash, 32); }
static bool flash_activate(void *ctx) { (void)ctx; ++activations; return !activate_failure; }
static void flash_abort(void *ctx) { (void)ctx; ++aborts; }
static kms_upload_t upload;
static kms_slave_history_t history;

static int remote_upload_tests(void)
{
    kms_remote_upload_t remote = {0};
    kms_management_frame_t q = {.slot = 2, .target = 22, .length = 4};
    kms_put_u32(q.data, 123);
    for (unsigned op = KMS_M_IDENTITY; op <= KMS_M_RESTART; ++op) {
        bool mutating = (op >= KMS_M_OTA_BEGIN && op <= KMS_M_OTA_ABORT) ||
            (op >= KMS_M_CACHE_BEGIN && op <= KMS_M_CACHE_INSTALL);
        CHECK(kms_management_changes_firmware(op) == mutating);
        if (mutating) continue;
        q.op = (uint8_t)op; kms_remote_upload_reply(&remote, &q, KMS_M_OK, 1000);
        CHECK(!kms_remote_upload_busy(&remote, 22, 1000));
    }
    q.op = KMS_M_OTA_BEGIN;
    kms_remote_upload_reply(&remote, &q, KMS_M_BUSY, 1000);
    CHECK(!kms_remote_upload_busy(&remote, 22, 1000));
    kms_remote_upload_reply(&remote, &q, KMS_M_CURRENT, 1000);
    CHECK(!kms_remote_upload_busy(&remote, 22, 1000));
    kms_remote_upload_reply(&remote, &q, KMS_M_OK, 1000);
    CHECK(kms_remote_upload_busy(&remote, 22, 1001));
    // Read-only progress/inspection never refresh an actual upload's lease.
    q.op = KMS_M_CACHE_PROGRESS; kms_remote_upload_reply(&remote, &q, KMS_M_OK, 119000);
    q.op = KMS_M_CACHE_INFO; kms_remote_upload_reply(&remote, &q, KMS_M_OK, 120000);
    CHECK(remote.at == 1000 && !kms_remote_upload_busy(&remote, 22, 121000));
    q.op = KMS_M_OTA_END; kms_remote_upload_reply(&remote, &q, KMS_M_OK, 121001);
    CHECK(kms_remote_upload_busy(&remote, 22, 121002)); // Verified image awaits activation.
    q.op = KMS_M_OTA_ACTIVATE;
    kms_remote_upload_reply(&remote, &q, KMS_M_STORAGE, 121003);
    CHECK(kms_remote_upload_busy(&remote, 22, 121003));
    kms_remote_upload_reply(&remote, &q, KMS_M_INTEGRITY, 121004);
    CHECK(kms_remote_upload_busy(&remote, 22, 121004));
    kms_remote_upload_reply(&remote, &q, KMS_M_OK, 121005);
    CHECK(!kms_remote_upload_busy(&remote, 22, 121005)); // No post-completion wait.
    q.op = KMS_M_OTA_BEGIN; kms_remote_upload_reply(&remote, &q, KMS_M_TIMEOUT, 200000);
    CHECK(kms_remote_upload_busy(&remote, 22, 200001)); // Lost ACK remains protected.
    q.op = KMS_M_OTA_ACTIVATE; kms_remote_upload_reply(&remote, &q, KMS_M_TIMEOUT, 200002);
    CHECK(kms_remote_upload_busy(&remote, 22, 200003));
    CHECK(kms_remote_upload_busy(&remote, 22, 200001 + KMS_MANAGEMENT_SESSION_MS));
    CHECK(!kms_remote_upload_busy(&remote, 22, 200002 + KMS_MANAGEMENT_SESSION_MS));
    CHECK(!kms_remote_upload_busy(&remote, 23, 200003)); // A new receiver boot ends the old upload.
    q.op = KMS_M_OTA_ABORT; kms_put_u32(q.data, 456);
    kms_remote_upload_reply(&remote, &q, KMS_M_EXPIRED, 200004);
    CHECK(kms_remote_upload_busy(&remote, 22, 200004)); // Another token cannot clear it.
    kms_put_u32(q.data, 123); kms_remote_upload_reply(&remote, &q, KMS_M_OK, 200005);
    CHECK(!kms_remote_upload_busy(&remote, 22, 200005));
    q.op = KMS_M_OTA_BEGIN; kms_remote_upload_reply(&remote, &q, KMS_M_OK, 300000);
    q.op = KMS_M_OTA_WRITE; kms_remote_upload_reply(&remote, &q, KMS_M_STORAGE, 300001);
    CHECK(!kms_remote_upload_busy(&remote, 22, 300001));
    q.op = KMS_M_OTA_BEGIN; kms_remote_upload_reply(&remote, &q, KMS_M_OK, 400000);
    q.op = KMS_M_OTA_END; kms_remote_upload_reply(&remote, &q, KMS_M_INTEGRITY, 400001);
    CHECK(!kms_remote_upload_busy(&remote, 22, 400001));
    q.op = KMS_M_OTA_BEGIN; kms_remote_upload_reply(&remote, &q, KMS_M_TIMEOUT, 0xfffffff0u);
    CHECK(kms_remote_upload_busy(&remote, 22, 0xfffffff0u + KMS_MANAGEMENT_SESSION_MS - 1));
    CHECK(!kms_remote_upload_busy(&remote, 22, 0xfffffff0u + KMS_MANAGEMENT_SESSION_MS));
    kms_remote_upload_t other = {0};
    kms_remote_upload_reply(&remote, &q, KMS_M_OK, 1000);
    q.target = 33; kms_remote_upload_reply(&other, &q, KMS_M_OK, 1000);
    q.op = KMS_M_OTA_ACTIVATE; kms_remote_upload_reply(&other, &q, KMS_M_OK, 1001);
    CHECK(kms_remote_upload_busy(&remote, 22, 1001) && !kms_remote_upload_busy(&other, 33, 1001));
    return 0;
}

__declspec(dllexport) int kms_run_management_tests(void)
{
    checks = 0;
    int failure = remote_upload_tests(); if (failure) return failure;
    CHECK(kms_crc32("123456789", 9) == 0xcbf43926u);
    CHECK(kms_crc32_extend(kms_crc32("1234", 4), "56789", 5) == 0xcbf43926u);
    kms_management_frame_t f = {.op = KMS_M_OTA_WRITE, .slot = 7, .id = 123,
        .sender = 456, .target = 789, .offset = 101, .length = KMS_M_DATA};
    for (unsigned n = 0; n < KMS_M_DATA; ++n) f.data[n] = (uint8_t)n;
    uint8_t wire[KMS_M_MAX_WIRE], damaged[KMS_M_MAX_WIRE];
    size_t length = kms_management_encode(&f, wire, sizeof(wire));
    CHECK(length == KMS_M_MAX_WIRE && length <= 250);
    kms_management_frame_t decoded;
    CHECK(kms_management_decode(&decoded, wire, length));
    CHECK(decoded.slot == 7 && decoded.id == 123 && decoded.sender == 456 && decoded.target == 789);
    CHECK(decoded.offset == 101 && decoded.length == KMS_M_DATA && !memcmp(decoded.data, f.data, KMS_M_DATA));
    // Every bit of metadata, payload and CRC is covered; truncated/extra bytes fail.
    for (size_t at = 0; at < length; ++at) for (unsigned bit = 0; bit < 8; ++bit) {
        memcpy(damaged, wire, length); damaged[at] ^= (uint8_t)(1u << bit);
        CHECK(!kms_management_decode(&decoded, damaged, length));
    }
    for (size_t size = 0; size < length; ++size) CHECK(!kms_management_decode(&decoded, wire, size));
    f.slot = 0; CHECK(!kms_management_encode(&f, wire, sizeof(wire))); f.slot = 1;
    f.length = KMS_M_DATA + 1; CHECK(!kms_management_encode(&f, wire, sizeof(wire))); f.length = 0;
    f.flags = 0x80; CHECK(!kms_management_encode(&f, wire, sizeof(wire))); f.flags = 0;
    char hex[17]; uint8_t bytes[8]; size_t bytes_count;
    kms_hex_encode((const uint8_t *)"KMSM", 4, hex);
    CHECK(kms_hex_decode(hex, bytes, sizeof(bytes), &bytes_count) && bytes_count == 4 && !memcmp(bytes, "KMSM", 4));
    CHECK(!kms_hex_decode("abc", bytes, sizeof(bytes), &bytes_count));
    CHECK(!kms_hex_decode("zz", bytes, sizeof(bytes), &bytes_count));
    CHECK(!kms_hex_decode("123456", bytes, 2, &bytes_count));
    kms_firmware_identity_t identity = {.git = "abcd-dirty", .utc = "2026-10-17T00:00:00Z", .image_bytes = 999, .chip = 2};
    kms_identity_encode(&identity, wire);
    CHECK(kms_identity_decode(&identity, wire, KMS_IDENTITY_BYTES) && identity.image_bytes == 999);
    memset(wire, 'x', 48); CHECK(!kms_identity_decode(&identity, wire, KMS_IDENTITY_BYTES));

    // Restart is bound to the current boot and a paired controller's request.
    // Repeated radio RPC retries acknowledge the same timer, never postpone it.
    kms_restart_t restart = {0};
    kms_management_frame_t request = {.op = KMS_M_RESTART, .slot = 2, .id = 10,
        .sender = 11, .target = 22, .length = 4};
    kms_put_u32(request.data, KMS_RESTART_KEY);
    length = kms_management_encode(&request, wire, sizeof(wire));
    CHECK(length && kms_management_decode(&decoded, wire, length) && decoded.op == KMS_M_RESTART);
    request.op = KMS_M_STREAM_STATUS + 1;
    CHECK(!kms_management_encode(&request, wire, sizeof(wire)));
    request.op = KMS_M_RESTART;
    CHECK(!kms_restart_take(&restart, 10000));
    CHECK(kms_restart_request(&restart, &request, 22, true, 100) == KMS_M_BUSY && !restart.pending);
    request.target = 0;
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_WRONG_BOOT);
    request.target = 21;
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_WRONG_BOOT);
    request.target = 22;
    request.data[0] ^= 1;
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_BAD_REQUEST);
    request.data[0] ^= 1;
    request.flags = KMS_M_FORCE;
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_BAD_REQUEST);
    request.flags = 0; request.offset = 1;
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_BAD_REQUEST);
    request.offset = 0; request.length = 0;
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_BAD_REQUEST);
    request.length = 4; request.sender = 0;
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_BAD_REQUEST && !restart.pending);
    request.sender = 11;
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_OK && restart.pending);
    CHECK(kms_restart_request(&restart, &request, 22, true, 900) == KMS_M_OK && restart.requested_at == 100);
    ++request.id;
    CHECK(kms_restart_request(&restart, &request, 22, false, 900) == KMS_M_BUSY);
    --request.id; ++request.sender;
    CHECK(kms_restart_request(&restart, &request, 22, false, 900) == KMS_M_BUSY);
    --request.sender;
    CHECK(!kms_restart_take(&restart, 100 + KMS_RESTART_DELAY_MS - 1));
    CHECK(kms_restart_take(&restart, 100 + KMS_RESTART_DELAY_MS) && !restart.pending);
    CHECK(!kms_restart_take(&restart, 100 + KMS_RESTART_DELAY_MS + 1));
    CHECK(kms_restart_request(&restart, &request, 22, false, 0xfffffff0u) == KMS_M_OK);
    CHECK(!kms_restart_take(&restart, 0xfffffff0u + KMS_RESTART_DELAY_MS - 1));
    CHECK(kms_restart_take(&restart, 0xfffffff0u + KMS_RESTART_DELAY_MS));
    request.length = 8; kms_put_u32(request.data + 4, KMS_RESTART_AUTOMATIC);
    CHECK(kms_restart_request(&restart, &request, 22, false, 100) == KMS_M_OK);
    CHECK(kms_restart_request(&restart, &request, 22, false, 200) == KMS_M_OK && restart.requested_at == 100);
    request.data[4] ^= 1;
    CHECK(kms_restart_request(&restart, &request, 22, false, 200) == KMS_M_BAD_REQUEST);
    CHECK(kms_slave_radio_detail(KMS_RF_RX_GAP, 3, 999999u) == (KMS_RADIO_FAULT_TAG | (KMS_RF_RX_GAP << 24) | (3u << 16) | 65535u));

    const kms_upload_io_t io = {flash_begin, flash_write, flash_verify, flash_activate, flash_abort, NULL};
    kms_upload_init(&upload, &io);
    memset(hash, 0xab, 32); memset(running_hash, 0xcd, 32);
    for (unsigned n = 0; n < sizeof(expected_image); ++n) expected_image[n] = (uint8_t)(n ^ 0x55);
    CHECK(kms_upload_begin(&upload, 1, 256, hash, false, hash, 512) == KMS_M_CURRENT && !begin_calls);
    CHECK(kms_upload_begin(&upload, 1, 256, hash, true, hash, 512) == KMS_M_OK && begin_calls == 1);
    CHECK(kms_upload_begin(&upload, 1, 256, hash, true, hash, 512) == KMS_M_OK && begin_calls == 1);
    CHECK(kms_upload_begin(&upload, 2, 256, hash, true, hash, 512) == KMS_M_BUSY);
    CHECK(kms_upload_write(&upload, 2, 0, expected_image, 100) == KMS_M_EXPIRED);
    CHECK(kms_upload_write(&upload, 1, 100, expected_image, 100) == KMS_M_OFFSET && !writes);
    CHECK(kms_upload_write(&upload, 1, 0, expected_image, 100) == KMS_M_OK && writes == 1);
    CHECK(kms_upload_write(&upload, 1, 0, expected_image, 100) == KMS_M_OK && writes == 1);
    uint8_t wrong[100] = {0}; CHECK(kms_upload_write(&upload, 1, 0, wrong, 100) == KMS_M_OFFSET);
    CHECK(kms_upload_end(&upload, 1) == KMS_M_OFFSET && !activations);
    CHECK(kms_upload_activate(&upload, 1) == KMS_M_INTEGRITY && !activations);
    CHECK(kms_upload_write(&upload, 1, 100, expected_image + 100, 156) == KMS_M_OK);
    CHECK(kms_upload_end(&upload, 1) == KMS_M_OK);
    CHECK(kms_upload_end(&upload, 1) == KMS_M_OK);
    activate_failure = true;
    CHECK(kms_upload_activate(&upload, 1) == KMS_M_STORAGE && !upload.activated);
    activate_failure = false;
    CHECK(kms_upload_activate(&upload, 1) == KMS_M_OK && upload.activated);
    CHECK(kms_upload_activate(&upload, 1) == KMS_M_OK && activations == 2);
    kms_upload_abort(&upload);
    CHECK(kms_upload_begin(&upload, 3, 513, hash, true, running_hash, 512) == KMS_M_INCOMPATIBLE);
    CHECK(kms_upload_begin(&upload, 0, 256, hash, true, running_hash, 512) == KMS_M_INCOMPATIBLE);
    CHECK(kms_upload_begin(&upload, 3, 256, hash, false, running_hash, 512) == KMS_M_OK);
    write_failure = true;
    CHECK(kms_upload_write(&upload, 3, 0, expected_image, 100) == KMS_M_STORAGE && !upload.active && aborts == 1);
    write_failure = false;
    CHECK(kms_upload_begin(&upload, 4, 256, hash, false, running_hash, 512) == KMS_M_OK);
    CHECK(kms_upload_write(&upload, 4, 0, expected_image, 128) == KMS_M_OK);
    CHECK(kms_upload_write(&upload, 4, 128, expected_image + 128, 128) == KMS_M_OK);
    corruption = true;
    CHECK(kms_upload_end(&upload, 4) == KMS_M_INTEGRITY && !upload.active && !upload.verified);
    CHECK(kms_upload_activate(&upload, 4) == KMS_M_EXPIRED && activations == 2);
    corruption = false;
    // Interrupted transfers never activate their partial image; a new session can start cleanly.
    CHECK(kms_upload_begin(&upload, 5, 256, hash, false, running_hash, 512) == KMS_M_OK);
    CHECK(kms_upload_write(&upload, 5, 0, expected_image, 128) == KMS_M_OK);
    kms_upload_abort(&upload);
    CHECK(kms_upload_write(&upload, 5, 128, expected_image + 128, 128) == KMS_M_EXPIRED);
    CHECK(kms_upload_begin(&upload, 6, 256, hash, false, running_hash, 512) == KMS_M_OK);
    kms_upload_abort(&upload);

    kms_slave_history_init(&history, 0, 55, 3);
    CHECK(kms_slave_history_valid(&history.journal));
    CHECK(kms_slave_history_event(&history, 1, KMS_S_USB_STALL, 0));
    CHECK(!kms_slave_history_event(&history, 2, KMS_S_USB_STALL, 0));
    CHECK(history.journal.event_count == 1 && history.journal.events[0].repeats == 2);
    CHECK(history.live.totals[KMS_S_USB_STALL] == 2);
    for (unsigned n = 0; n < KMS_SLAVE_HISTORY_EVENTS + 3; ++n)
        kms_slave_history_event(&history, 3 + n, KMS_S_USB_OVERFLOW, n);
    CHECK(history.journal.event_count == KMS_SLAVE_HISTORY_EVENTS && history.journal.events_overwritten == 4);
    CHECK(history.live.totals[KMS_S_USB_STALL] == 2 && history.live.totals[KMS_S_USB_OVERFLOW] == KMS_SLAVE_HISTORY_EVENTS + 3);
    for (unsigned n = 0; n < KMS_SLAVE_HISTORY_STATS + 2; ++n) kms_slave_history_checkpoint(&history);
    CHECK(history.journal.stats_count == KMS_SLAVE_HISTORY_STATS && history.journal.stats_overwritten == 2);
    kms_slave_history_clear(&history);
    CHECK(!history.journal.event_count && !history.journal.stats_count && !history.journal.events_overwritten);
    CHECK(history.live.totals[KMS_S_USB_STALL] == 2 && history.live.boot == 55);
    CHECK(kms_slave_history_valid(&history.journal));
    history.journal.next_event = KMS_SLAVE_HISTORY_EVENTS;
    CHECK(!kms_slave_history_valid(&history.journal));
    kms_slave_history_init(&history, 0xfffffff0u, 55, 3);
    kms_slave_history_tick(&history, 20);
    CHECK(history.live.uptime_ms == 36);
    CHECK(kms_boot_health_decide(100, 3099, true, true, true, 3000, 30000) == KMS_BOOT_WAIT);
    CHECK(kms_boot_health_decide(100, 3100, true, true, true, 3000, 30000) == KMS_BOOT_CONFIRM);
    CHECK(kms_boot_health_decide(100, 3100, false, true, true, 3000, 30000) == KMS_BOOT_WAIT);
    CHECK(kms_boot_health_decide(100, 3100, true, false, true, 3000, 30000) == KMS_BOOT_WAIT);
    CHECK(kms_boot_health_decide(100, 3100, true, true, false, 3000, 30000) == KMS_BOOT_WAIT);
    CHECK(kms_boot_health_decide(100, 30100, true, true, true, 3000, 30000) == KMS_BOOT_ROLLBACK);
    CHECK(kms_boot_health_decide(100, 30100, false, false, false, 3000, 30000) == KMS_BOOT_ROLLBACK);
    CHECK(kms_boot_health_decide(0xfffffff0u, 20, true, true, true, 30, 100) == KMS_BOOT_CONFIRM);
    return 0;
}
__declspec(dllexport) int kms_management_test_count(void) { return checks; }
__declspec(dllexport) unsigned kms_management_test_echo(const uint8_t *in, unsigned size, uint8_t *out)
{
    kms_management_frame_t frame;
    return kms_management_decode(&frame, in, size) ? (unsigned)kms_management_encode(&frame, out, KMS_M_MAX_WIRE) : 0;
}
