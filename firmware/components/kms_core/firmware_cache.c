#include "kms_firmware_cache.h"
#include <string.h>

static bool valid_identity(const kms_firmware_identity_t *identity)
{
    return identity->chip == 2 && identity->image_bytes >= 320 &&
        identity->image_bytes <= KMS_CACHE_IMAGE_MAX &&
        !identity->git[sizeof(identity->git) - 1] && !identity->utc[sizeof(identity->utc) - 1];
}

void kms_cache_header_encode(uint8_t *header, const kms_firmware_identity_t *identity, uint32_t generation)
{
    memset(header, 0, KMS_CACHE_HEADER_BYTES);
    memcpy(header, "KMSC", 4);
    kms_put_u32(header + 4, 1);
    kms_put_u32(header + 8, generation);
    kms_put_u32(header + 12, KMS_CACHE_DATA_OFFSET);
    kms_identity_encode(identity, header + 16);
    kms_put_u32(header + 136, kms_crc32(header, 136));
    memcpy(header + 140, "DONE", 4);
}

bool kms_cache_header_decode(const uint8_t *header, kms_firmware_identity_t *identity, uint32_t *generation)
{
    if (memcmp(header, "KMSC", 4) || kms_get_u32(header + 4) != 1 ||
        kms_get_u32(header + 12) != KMS_CACHE_DATA_OFFSET || memcmp(header + 140, "DONE", 4) ||
        kms_get_u32(header + 136) != kms_crc32(header, 136) ||
        !kms_identity_decode(identity, header + 16, KMS_IDENTITY_BYTES) || !valid_identity(identity)) return false;
    *generation = kms_get_u32(header + 8);
    return true;
}

static bool begin(void *ctx, uint32_t size)
{
    kms_firmware_cache_t *cache = ctx;
    cache->destination = cache->current == 0 ? 1 : 0;
    uint32_t bytes = (KMS_CACHE_DATA_OFFSET + size + 4095u) & ~4095u;
    return cache->io.erase(cache->io.ctx, cache->destination, bytes);
}

static bool write_chunk(void *ctx, uint32_t offset, const uint8_t *data, size_t bytes)
{
    kms_firmware_cache_t *cache = ctx;
    return cache->io.write(cache->io.ctx, cache->destination, KMS_CACHE_DATA_OFFSET + offset, data, bytes);
}

static bool verify(void *ctx, uint32_t size, const uint8_t *hash)
{
    kms_firmware_cache_t *cache = ctx;
    return size == cache->pending.image_bytes && !memcmp(hash, cache->pending.sha256, 32) &&
        cache->io.verify(cache->io.ctx, cache->destination, &cache->pending);
}

static bool commit(void *ctx)
{
    kms_firmware_cache_t *cache = ctx;
    uint8_t header[KMS_CACHE_HEADER_BYTES], actual[KMS_CACHE_HEADER_BYTES];
    kms_cache_header_encode(header, &cache->pending, cache->generation + 1);
    // The marker is written last. An interrupted header never replaces the old slot.
    if (!cache->io.write(cache->io.ctx, cache->destination, 0, header, 140) ||
        !cache->io.write(cache->io.ctx, cache->destination, 140, header + 140, 4) ||
        !cache->io.read(cache->io.ctx, cache->destination, 0, actual, sizeof(actual)) ||
        memcmp(header, actual, sizeof(header))) return false;
    cache->current = (int)cache->destination;
    cache->identity = cache->pending;
    ++cache->generation;
    return true;
}

static void abort_upload(void *ctx) { (void)ctx; }

void kms_firmware_cache_init(kms_firmware_cache_t *cache, const kms_firmware_cache_io_t *io)
{
    memset(cache, 0, sizeof(*cache));
    cache->current = -1;
    cache->io = *io;
    for (unsigned slot = 0; slot < 2; ++slot) {
        uint8_t header[KMS_CACHE_HEADER_BYTES];
        kms_firmware_identity_t identity;
        uint32_t generation;
        if (!io->read(io->ctx, slot, 0, header, sizeof(header)) ||
            !kms_cache_header_decode(header, &identity, &generation) ||
            !io->verify(io->ctx, slot, &identity)) continue;
        if (cache->current < 0 || (int32_t)(generation - cache->generation) > 0) {
            cache->current = (int)slot;
            cache->identity = identity;
            cache->generation = generation;
        }
    }
    const kms_upload_io_t upload_io = {begin, write_chunk, verify, commit, abort_upload, cache};
    kms_upload_init(&cache->upload, &upload_io);
}

unsigned kms_firmware_cache_begin(kms_firmware_cache_t *cache, uint32_t token,
                                  const kms_firmware_identity_t *identity, bool force)
{
    if (!valid_identity(identity)) return KMS_M_INCOMPATIBLE;
    if (cache->upload.active && memcmp(identity, &cache->pending, sizeof(*identity))) return KMS_M_BUSY;
    unsigned result = kms_upload_begin(&cache->upload, token, identity->image_bytes, identity->sha256,
        force || cache->current < 0, cache->identity.sha256, KMS_CACHE_IMAGE_MAX);
    if (result == KMS_M_OK) cache->pending = *identity;
    return result;
}

unsigned kms_firmware_cache_end(kms_firmware_cache_t *cache, uint32_t token)
{
    unsigned result = kms_upload_end(&cache->upload, token);
    if (result == KMS_M_OK) result = kms_upload_activate(&cache->upload, token);
    if (result == KMS_M_OK) kms_upload_abort(&cache->upload);
    return result;
}

bool kms_firmware_cache_read(kms_firmware_cache_t *cache, uint32_t offset, void *data, size_t bytes)
{
    return cache->current >= 0 && offset <= cache->identity.image_bytes &&
        bytes <= cache->identity.image_bytes - offset &&
        cache->io.read(cache->io.ctx, (unsigned)cache->current, KMS_CACHE_DATA_OFFSET + offset, data, bytes);
}
