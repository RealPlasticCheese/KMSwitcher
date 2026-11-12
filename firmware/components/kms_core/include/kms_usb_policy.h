#pragma once
#include "kms_core.h"

enum { KMS_USB_RULES = 24, KMS_USB_CHANNELS = 8,
       KMS_USB_KEYBOARD = 1, KMS_USB_MOUSE = 2, KMS_USB_CONSUMER = 4,
       KMS_USB_SYSTEM = 8, KMS_USB_LEDS = 16 };
typedef struct {
    uint32_t serial;
    uint16_t vid, pid;
    uint8_t interface_number, enabled, reserved[2];
} kms_usb_rule_t;
typedef struct {
    uint32_t schema;
    uint8_t automatic, count, reserved[2];
    kms_usb_rule_t rules[KMS_USB_RULES];
} kms_usb_policy_t;

void kms_usb_policy_reset(kms_usb_policy_t *, bool automatic);
bool kms_usb_policy_valid(const kms_usb_policy_t *);
bool kms_usb_policy_enabled(const kms_usb_policy_t *, const kms_usb_rule_t *);
bool kms_usb_policy_set(kms_usb_policy_t *, kms_usb_rule_t);
uint8_t kms_usb_hid_kinds(const kms_hid_parser_t *);

enum { KMS_USB_ACTIVE_DEVICES = 2 };
typedef struct {
    uint8_t device, endpoints, kinds;
    bool enabled, preferred, active;
} kms_usb_candidate_t;
// Choose complete supported input bundles from at most two physical devices.
// No vendor/model identities participate in automatic ranking.
uint32_t kms_usb_plan(const kms_usb_candidate_t *, unsigned count, unsigned baseline);
int kms_usb_policy_override(const kms_usb_policy_t *, const kms_usb_rule_t *);
