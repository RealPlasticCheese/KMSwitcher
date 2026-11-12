#pragma once
#include "kms_management.h"

void kms_receiver_cache_init(void);
bool kms_receiver_cache_available(void);
bool kms_receiver_cache_busy(void);
const kms_firmware_identity_t *kms_receiver_cache_identity(void);
bool kms_receiver_cache_read(uint32_t, void *, size_t);
void kms_receiver_cache_request(const kms_management_frame_t *, kms_management_frame_t *);
void kms_receiver_cache_tick(uint32_t);
