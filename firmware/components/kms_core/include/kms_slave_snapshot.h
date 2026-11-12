#pragma once
#include "kms_slave_history.h"
#include "kms_management.h"

// Frozen binary data plus block offsets; never retain the formatted text.
enum { KMS_SLAVE_TEXT_BLOCKS = 3 + KMS_SLAVE_HISTORY_EVENTS + KMS_SLAVE_HISTORY_STATS };
typedef struct {
    kms_slave_history_t history;
    kms_firmware_identity_t identity;
    uint32_t offsets[KMS_SLAVE_TEXT_BLOCKS + 1];
    uint32_t blocks, size, crc;
} kms_slave_snapshot_t;
bool kms_slave_snapshot_prepare(kms_slave_snapshot_t *);
size_t kms_slave_snapshot_read(const kms_slave_snapshot_t *, uint32_t offset, void *, size_t capacity);
