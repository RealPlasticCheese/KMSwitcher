#include "kms_receiver_cache.h"
#include "kms_firmware_cache.h"
#include "kms_build.h"
#include "kms_runtime.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include <string.h>

static kms_firmware_cache_t cache;
static const esp_partition_t *slots[2];
static bool available;
static uint32_t owner, last_activity;

static bool read_slot(void *ctx, unsigned slot, uint32_t offset, void *data, size_t bytes)
{ (void)ctx; return slots[slot] && esp_partition_read(slots[slot], offset, data, bytes) == ESP_OK; }
static bool erase_slot(void *ctx, unsigned slot, uint32_t bytes)
{ (void)ctx; return esp_partition_erase_range(slots[slot], 0, bytes) == ESP_OK; }
static bool write_slot(void *ctx, unsigned slot, uint32_t offset, const void *data, size_t bytes)
{ (void)ctx; return esp_partition_write(slots[slot], offset, data, bytes) == ESP_OK; }

static bool verify_slot(void *ctx, unsigned slot, const kms_firmware_identity_t *identity)
{
    (void)ctx;
    uint8_t prefix[32 + sizeof(esp_app_desc_t)], hash[32], appended[32];
    uint32_t size = identity->image_bytes;
    if (size < sizeof(prefix) + 32 || size > KMS_CACHE_IMAGE_MAX ||
        !read_slot(NULL, slot, KMS_CACHE_DATA_OFFSET, prefix, sizeof(prefix)) ||
        prefix[0] != 0xe9 || prefix[12] != 2 || prefix[13] || prefix[23] != 1) return false;
    esp_app_desc_t app;
    memcpy(&app, prefix + 32, sizeof(app));
    if (app.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(app.project_name, "kmswitcher_receiver", sizeof("kmswitcher_receiver"))) return false;
    if (!kms_build_hash_partition_range(slots[slot], KMS_CACHE_DATA_OFFSET, size, hash) ||
        memcmp(hash, identity->sha256, 32)) return false;
    return read_slot(NULL, slot, KMS_CACHE_DATA_OFFSET + size - 32, appended, sizeof(appended)) &&
        kms_build_hash_partition_range(slots[slot], KMS_CACHE_DATA_OFFSET, size - 32, hash) &&
        !memcmp(hash, appended, 32);
}

void kms_receiver_cache_init(void)
{
    slots[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "s2_cache_0");
    slots[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "s2_cache_1");
    available = slots[0] && slots[1] && slots[0]->size == KMS_CACHE_SLOT_BYTES && slots[1]->size == KMS_CACHE_SLOT_BYTES;
    if (available) {
        const kms_firmware_cache_io_t io = {read_slot, erase_slot, write_slot, verify_slot, NULL};
        kms_firmware_cache_init(&cache, &io);
    }
}
bool kms_receiver_cache_available(void) { return available; }
bool kms_receiver_cache_busy(void) { return available && cache.upload.active; }
const kms_firmware_identity_t *kms_receiver_cache_identity(void)
{ return available && cache.current >= 0 ? &cache.identity : NULL; }
bool kms_receiver_cache_read(uint32_t offset, void *data, size_t bytes)
{ return available && kms_firmware_cache_read(&cache, offset, data, bytes); }

void kms_receiver_cache_request(const kms_management_frame_t *q, kms_management_frame_t *r)
{
    r->status = KMS_M_BAD_REQUEST;
    if (!available) { r->status = KMS_M_UNSUPPORTED; return; }
    if (q->op == KMS_M_CACHE_INFO && !q->length) {
        if (cache.current < 0) r->status = KMS_M_EXPIRED;
        else { kms_identity_encode(&cache.identity, r->data); r->length = KMS_IDENTITY_BYTES; r->status = KMS_M_OK; }
        return;
    }
    if (cache.upload.active && q->sender != owner) { r->status = KMS_M_BUSY; return; }
    uint32_t token = q->length >= 4 ? kms_get_u32(q->data) : 0;
    if (q->op == KMS_M_CACHE_BEGIN && q->length == KMS_IDENTITY_BYTES + 4) {
        kms_firmware_identity_t identity;
        if (!kms_identity_decode(&identity, q->data + 4, KMS_IDENTITY_BYTES)) return;
        r->status = (uint8_t)kms_firmware_cache_begin(&cache, token, &identity, (q->flags & KMS_M_FORCE) != 0);
        if (r->status == KMS_M_OK) owner = q->sender;
    } else if (q->op == KMS_M_CACHE_WRITE && q->length > 4) {
        r->status = (uint8_t)kms_upload_write(&cache.upload, token, q->offset, q->data + 4, q->length - 4);
    } else if (q->op == KMS_M_CACHE_END && q->length == 4) {
        r->status = (uint8_t)kms_firmware_cache_end(&cache, token);
    } else if (q->op == KMS_M_CACHE_ABORT && q->length == 4) {
        if (!cache.upload.active || token != cache.upload.token) r->status = KMS_M_EXPIRED;
        else { kms_upload_abort(&cache.upload); r->status = KMS_M_OK; }
    }
    if (r->status == KMS_M_OK) last_activity = kms_now();
    r->length = 4; kms_put_u32(r->data, cache.upload.offset);
}

void kms_receiver_cache_tick(uint32_t now)
{
    if (kms_receiver_cache_busy() && kms_time_elapsed(now, last_activity, KMS_MANAGEMENT_SESSION_MS))
        kms_upload_abort(&cache.upload);
}
