#pragma once
#include "kms_core.h"
// Live state belongs to the USB-owner task. Owner is the HID handle.
typedef struct {
    struct { unsigned address, number, completed, failed; bool busy, retiring; } outputs[KMS_USB_HID_INTERFACES];
    unsigned count;
    uint8_t desired;
    bool enabled, known;
} kms_usb_led_status_t;
void kms_usb_leds_snapshot(kms_usb_led_status_t *);
void kms_usb_leds_print_status(const kms_usb_led_status_t *);
void kms_usb_leds_init(void);
void kms_usb_leds_set_enabled(bool enabled);
void kms_usb_leds_attach(void *owner, uint8_t address, uint8_t interface_number, const kms_hid_parser_t *);
void kms_usb_leds_detach(void *owner);
void kms_usb_leds_tick(bool known, uint8_t leds);
void kms_usb_leds_status(void);
void kms_usb_leds_quiesce(bool quiescing);
bool kms_usb_leds_drained(void);
