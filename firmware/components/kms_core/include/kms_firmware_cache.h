#pragma once
#include "kms_management.h"

// Each slot has one header sector followed by an ordinary downloadable S2 .bin.
// Generation, identity and CRC are committed only after payload verification.
enum { KMS_CACHE_HEADER_BYTES = 144, KMS_CACHE_DATA_OFFSET = 4096,
       KMS_CACHE_SLOT_BYTES = 0x200000, KMS_CACHE_IMAGE_MAX = 0x1e0000 };

typedef struct {
    bool (*read)(void *, unsigned, uint32_t, void *, size_t);
    bool (*erase)(void *, unsigned, uint32_t);
    bool (*write)(void *, unsigned, uint32_t, const void *, size_t);
    bool (*verify)(void *, unsigned, const kms_firmware_identity_t *);
    void *ctx;
} kms_firmware_cache_io_t;

typedef struct {
    kms_firmware_cache_io_t io;
    kms_upload_t upload;
    kms_firmware_identity_t identity, pending;
    uint32_t generation;
    int current;
    unsigned destination;
} kms_firmware_cache_t;

bool kms_cache_header_decode(const uint8_t *, kms_firmware_identity_t *, uint32_t *);
void kms_cache_header_encode(uint8_t *, const kms_firmware_identity_t *, uint32_t);
void kms_firmware_cache_init(kms_firmware_cache_t *, const kms_firmware_cache_io_t *);
unsigned kms_firmware_cache_begin(kms_firmware_cache_t *, uint32_t, const kms_firmware_identity_t *, bool);
unsigned kms_firmware_cache_end(kms_firmware_cache_t *, uint32_t);
bool kms_firmware_cache_read(kms_firmware_cache_t *, uint32_t, void *, size_t);
