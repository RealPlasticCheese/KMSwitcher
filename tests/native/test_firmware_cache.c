#include "kms_firmware_cache.h"
#include "kms_cached_update.h"
#include <string.h>

#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, writes, fail_write;
static uint8_t flash[2][8192];
static bool corrupt;
static bool read_slot(void *ctx, unsigned slot, uint32_t offset, void *data, size_t bytes)
{ (void)ctx; if (offset + bytes > sizeof(flash[slot])) return false; memcpy(data, flash[slot] + offset, bytes); return true; }
static bool erase_slot(void *ctx, unsigned slot, uint32_t bytes)
{ (void)ctx; if (bytes > sizeof(flash[slot])) return false; memset(flash[slot], 255, bytes); writes = 0; return true; }
static bool write_slot(void *ctx, unsigned slot, uint32_t offset, const void *data, size_t bytes)
{
    (void)ctx;
    if (++writes == fail_write || offset + bytes > sizeof(flash[slot])) return false;
    const uint8_t *source = data;
    for (size_t i = 0; i < bytes; ++i) flash[slot][offset + i] &= source[i];
    return true;
}
static bool verify_slot(void *ctx, unsigned slot, const kms_firmware_identity_t *identity)
{
    (void)ctx;
    if (corrupt || identity->image_bytes != 512) return false;
    for (unsigned i = 0; i < 512; ++i)
        if (flash[slot][KMS_CACHE_DATA_OFFSET + i] != identity->sha256[0]) return false;
    return true;
}
static const kms_firmware_cache_io_t cache_io = {read_slot, erase_slot, write_slot, verify_slot, NULL};
static kms_firmware_identity_t image_identity(uint8_t seed)
{
    kms_firmware_identity_t image = {.git = "revision", .utc = "2026-10-28T00:00:00Z", .chip = 2, .image_bytes = 512, .capabilities = 3};
    memset(image.sha256, seed, 32); return image;
}
static unsigned upload_image(kms_firmware_cache_t *cache, uint8_t seed)
{
    kms_firmware_identity_t image = image_identity(seed);
    unsigned result = kms_firmware_cache_begin(cache, 123, &image, false);
    if (result != KMS_M_OK) return result;
    uint8_t bytes[188]; memset(bytes, seed, sizeof(bytes));
    for (unsigned offset = 0; offset < 512; offset += 188) {
        unsigned count = 512 - offset; if (count > 188) count = 188;
        result = kms_upload_write(&cache->upload, 123, offset, bytes, count);
        if (result != KMS_M_OK) return result;
    }
    return kms_firmware_cache_end(cache, 123);
}

__declspec(dllexport) int kms_run_firmware_cache_tests(void)
{
    checks = writes = fail_write = 0; corrupt = false; memset(flash, 255, sizeof(flash));
    kms_firmware_cache_t cache, reboot;
    kms_firmware_cache_init(&cache, &cache_io);
    CHECK(cache.current == -1);
    CHECK(upload_image(&cache, 11) == KMS_M_OK);
    CHECK(cache.current == 0 && cache.generation == 1 && !cache.upload.active);
    kms_firmware_cache_init(&reboot, &cache_io);
    CHECK(reboot.current == 0 && reboot.identity.sha256[0] == 11);
    kms_firmware_identity_t identity = image_identity(11);
    CHECK(kms_firmware_cache_begin(&cache, 123, &identity, false) == KMS_M_CURRENT);
    identity.chip = 9;
    CHECK(kms_firmware_cache_begin(&cache, 123, &identity, false) == KMS_M_INCOMPATIBLE);
    identity = image_identity(22);
    CHECK(kms_firmware_cache_begin(&cache, 456, &identity, false) == KMS_M_OK);
    uint8_t bytes[188]; memset(bytes, 22, sizeof(bytes));
    CHECK(kms_upload_write(&cache.upload, 456, 0, bytes, 188) == KMS_M_OK);
    unsigned before = writes;
    CHECK(kms_upload_write(&cache.upload, 456, 0, bytes, 188) == KMS_M_OK && writes == before);
    CHECK(kms_firmware_cache_end(&cache, 456) == KMS_M_OFFSET);
    // Power loss before commit retains the original image, including after a partial payload.
    kms_firmware_cache_init(&reboot, &cache_io);
    CHECK(reboot.current == 0 && reboot.identity.sha256[0] == 11);
    kms_upload_abort(&cache.upload);
    for (unsigned cut = 1; cut <= 5; ++cut) {
        fail_write = cut;
        CHECK(upload_image(&cache, 22) != KMS_M_OK);
        kms_firmware_cache_init(&reboot, &cache_io);
        CHECK(reboot.current == 0 && reboot.identity.sha256[0] == 11);
        kms_upload_abort(&cache.upload);
    }
    fail_write = 0; corrupt = true;
    CHECK(upload_image(&cache, 22) == KMS_M_INTEGRITY);
    corrupt = false;
    CHECK(upload_image(&cache, 22) == KMS_M_OK && cache.current == 1 && cache.generation == 2);
    kms_firmware_cache_init(&reboot, &cache_io);
    CHECK(reboot.current == 1 && reboot.identity.sha256[0] == 22);
    flash[1][KMS_CACHE_DATA_OFFSET + 1] ^= 1;
    kms_firmware_cache_init(&reboot, &cache_io);
    CHECK(reboot.current == 0 && reboot.identity.sha256[0] == 11);
    uint8_t out[512];
    CHECK(kms_firmware_cache_read(&reboot, 0, out, sizeof(out)) && out[0] == 11);
    CHECK(!kms_firmware_cache_read(&reboot, 512, out, 1));
    CHECK(!kms_firmware_cache_read(&reboot, 0xffffffffu, out, 1));
    uint8_t header[KMS_CACHE_HEADER_BYTES]; uint32_t generation;
    identity = image_identity(11);
    kms_cache_header_encode(header, &identity, 0xffffffffu);
    CHECK(kms_cache_header_decode(header, &identity, &generation) && generation == 0xffffffffu);
    for (unsigned i = 0; i < sizeof(header); ++i) {
        header[i] ^= 1; CHECK(!kms_cache_header_decode(header, &identity, &generation)); header[i] ^= 1;
    }
    return 0;
}

static unsigned scenario, begins, remote_writes, remote_aborts;
static uint32_t remote_boot, expected_offset;
static bool activated;
static kms_firmware_identity_t target_image;
static unsigned remote_exchange(void *ctx, kms_management_frame_t *q, kms_management_frame_t *r)
{
    (void)ctx; r->sender = remote_boot;
    if (q->op == KMS_M_IDENTITY) {
        kms_firmware_identity_t running = image_identity(activated && scenario != 2 ? 22 : 11);
        if (scenario == 3) running = target_image;
        running.capabilities = 7;
        kms_identity_encode(&running, r->data); r->length = 120; return KMS_M_OK;
    }
    if (q->target != remote_boot) return KMS_M_WRONG_BOOT;
    if (q->op == KMS_M_OTA_BEGIN) {
        ++begins;
        if (scenario == 1 && begins < 3) return KMS_M_BUSY;
        kms_put_u32(r->data, 0); r->length = 4; return KMS_M_OK;
    }
    if (q->op == KMS_M_OTA_WRITE) {
        ++remote_writes;
        if (scenario == 4) return KMS_M_WRONG_BOOT;
        if (q->offset != expected_offset) return KMS_M_OFFSET;
        expected_offset += q->length - 4;
        kms_put_u32(r->data, scenario == 5 ? 999 : expected_offset); r->length = 4; return KMS_M_OK;
    }
    if (q->op == KMS_M_OTA_ACTIVATE) {
        activated = true;
        if (scenario != 6) ++remote_boot;
        return scenario == 1 ? KMS_M_TIMEOUT : KMS_M_OK;
    }
    if (q->op == KMS_M_OTA_ABORT) ++remote_aborts;
    return KMS_M_OK;
}
static bool read_image(void *ctx, uint32_t offset, void *data, size_t bytes)
{ (void)ctx; (void)offset; memset(data, 22, bytes); return scenario != 7; }

static unsigned window_count;
static unsigned exchange_window(void *ctx, kms_management_frame_t *frames, unsigned count)
{
    ++window_count;
    if (!count || count > 4) return KMS_M_BAD_REQUEST;
    for (unsigned i = 0; i < count; ++i) {
        kms_management_frame_t reply = {0};
        unsigned result = remote_exchange(ctx, &frames[i], &reply);
        if (result != KMS_M_OK) return result;
    }
    return KMS_M_OK;
}
__declspec(dllexport) int kms_run_cached_update_tests(void)
{
    const kms_cached_update_io_t io = {remote_exchange, read_image, NULL};
    target_image = image_identity(22);
    for (scenario = 0; scenario <= 7; ++scenario) {
        remote_boot = 50; begins = remote_writes = remote_aborts = expected_offset = 0; activated = false;
        kms_cached_update_t update;
        kms_cached_update_start(&update, &io, &target_image, 3, false, 123);
        for (unsigned now = 100; now < 65000 && kms_cached_update_busy(&update); now += 100)
            kms_cached_update_step(&update, now);
        if (scenario < 2) {
            CHECK(update.state == KMS_CACHED_UPDATED && expected_offset == 512 && remote_boot == 51);
            CHECK(begins == (scenario == 1 ? 3u : 1u));
        } else if (scenario == 3) CHECK(update.state == KMS_CACHED_SKIPPED && !remote_writes);
        else CHECK(update.state == KMS_CACHED_FAILED);
        if (scenario == 4 || scenario == 5 || scenario == 7) CHECK(remote_aborts == 1);
        if (scenario == 6) CHECK(update.result == KMS_M_TIMEOUT);
    }
    // Forced identical images still need a different confirmed boot.
    scenario = 3; activated = false; remote_boot = 50; expected_offset = 0;
    kms_cached_update_t forced;
    kms_cached_update_start(&forced, &io, &target_image, 3, true, 456);
    for (unsigned now = 100; now < 10000 && kms_cached_update_busy(&forced); now += 100)
        kms_cached_update_step(&forced, now);
    CHECK(forced.state == KMS_CACHED_UPDATED && remote_boot == 51);
    kms_cached_update_io_t windowed = io; windowed.exchange_window = exchange_window;
    scenario = 0; activated = false; remote_boot = 50; expected_offset = 0; window_count = 0;
    kms_cached_update_start(&forced, &windowed, &target_image, 3, false, 789);
    for (unsigned now = 100; now < 10000 && kms_cached_update_busy(&forced); now += 100)
        kms_cached_update_step(&forced, now);
    CHECK(forced.state == KMS_CACHED_UPDATED && expected_offset == 512 && window_count == 1);
    return 0;
}
__declspec(dllexport) unsigned kms_firmware_cache_test_count(void) { return checks; }
__declspec(dllexport) unsigned kms_cache_test_decode(const uint8_t *header)
{
    kms_firmware_identity_t identity; uint32_t generation;
    return kms_cache_header_decode(header, &identity, &generation) ? identity.image_bytes : 0;
}
