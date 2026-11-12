#pragma once
#include "kms_core.h"
void kms_usb_device_init(void);
void kms_usb_device_tick(void);
bool kms_usb_device_ready(void);
bool kms_usb_device_submit(const kms_sample_t *);
bool kms_usb_device_submit_many(const kms_sample_t *, unsigned count);
void kms_usb_device_clear(void);
bool kms_usb_device_take_fault(void);
bool kms_usb_device_suspended(void);
void kms_usb_device_keyboard_leds(kms_led_state_t *);
