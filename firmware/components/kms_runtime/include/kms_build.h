#pragma once
#include "kms_management.h"
#include "esp_partition.h"

bool kms_build_init(void);
const kms_firmware_identity_t *kms_build_identity(void);
bool kms_build_hash_partition(const esp_partition_t *, uint32_t, uint8_t *);
bool kms_build_hash_partition_range(const esp_partition_t *, uint32_t, uint32_t, uint8_t *);
