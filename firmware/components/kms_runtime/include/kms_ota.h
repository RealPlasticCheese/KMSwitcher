#pragma once
#include "kms_management.h"
void kms_ota_init(bool master);
void kms_ota_request(const kms_management_frame_t *, kms_management_frame_t *);
void kms_ota_tick(bool app_alive, bool usb_initialized, bool radio_seen);
bool kms_ota_confirmed(void);
bool kms_ota_busy(void);
void kms_ota_startup_failed(void);
