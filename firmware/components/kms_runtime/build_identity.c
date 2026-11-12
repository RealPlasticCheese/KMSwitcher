#include "kms_build.h"
#include "kms_build_identity.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <string.h>

static kms_firmware_identity_t identity = {
    .git = KMS_BUILD_GIT, .utc = KMS_BUILD_UTC, .chip = CONFIG_IDF_FIRMWARE_CHIP_ID
};
bool kms_build_hash_partition(const esp_partition_t *part, uint32_t size, uint8_t *hash)
{ return kms_build_hash_partition_range(part, 0, size, hash); }
bool kms_build_hash_partition_range(const esp_partition_t *part, uint32_t start, uint32_t size, uint8_t *hash)
{
    if (!part || !size || start > part->size || size > part->size - start) return false;
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    uint8_t block[1024];
    for (uint32_t offset = 0; offset < size;) {
        size_t count = size - offset;
        if (count > sizeof(block)) count = sizeof(block);
        if (esp_partition_read(part, start + offset, block, count) != ESP_OK ||
            psa_hash_update(&operation, block, count) != PSA_SUCCESS) {
            psa_hash_abort(&operation); return false;
        }
        offset += (uint32_t)count;
    }
    size_t written;
    if (psa_hash_finish(&operation, hash, 32, &written) != PSA_SUCCESS) {
        psa_hash_abort(&operation); return false;
    }
    return written == 32;
}
bool kms_build_init(void)
{
    _Static_assert(sizeof(KMS_BUILD_GIT) <= KMS_GIT_BYTES, "Git identity too long");
    _Static_assert(sizeof(KMS_BUILD_UTC) <= KMS_UTC_BYTES, "UTC identity too long");
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;
    esp_partition_pos_t position = {.offset = running->address, .size = running->size};
    esp_image_metadata_t metadata;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK) return false;
    identity.image_bytes = metadata.image_len;
    if (!kms_build_hash_partition(running, identity.image_bytes, identity.sha256)) return false;
    identity.capabilities = 1 | KMS_CAP_RESTART | KMS_CAP_RADIO_DIAGNOSTICS | 64 | KMS_CAP_RADIO_PROBE;
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    if (esp_ota_get_next_update_partition(NULL)) identity.capabilities |= 2;
#endif
    return true;
}
const kms_firmware_identity_t *kms_build_identity(void) { return &identity; }
