#pragma once
#include "kms_desktop_runtime.h"

void kms_device_controls_init(void);
void kms_device_controls_tick(void);
// Runs on the USB owner task; never calls link/router functions.
void kms_device_usb_request(kms_desktop_message_t *, unsigned source, uint32_t boot);
