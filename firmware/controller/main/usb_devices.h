#pragma once
#include "kms_usb_policy.h"

enum { KMS_USB_DEVICES = 8, KMS_USB_INTERFACES = 24 };
typedef struct kms_usb_interface kms_usb_interface_t;
typedef struct {
    void (*attached)(kms_usb_interface_t *, const kms_hid_parser_t *);
    bool (*report)(kms_usb_interface_t *, const kms_hid_parser_t *, const uint8_t *, size_t, uint32_t captured_at);
    void (*removed)(kms_usb_interface_t *);
    void (*overflow)(void);
} kms_usb_input_callbacks_t;
typedef struct {
    uint32_t serial;
    uint16_t vid, pid;
    uint8_t address, parent, port, speed, hub_endpoints;
    char name[48];
} kms_usb_device_info_t;
typedef struct {
    uint32_t reports, decoded;
    uint32_t last_submit, last_complete;
    int last_status, last_bytes;
    uint16_t descriptor_bytes;
    uint8_t address, number, device_class, endpoints, kinds, state;
    uint8_t endpoint;
    bool wanted, transfer_pending, completed_once;
} kms_usb_interface_info_t;
enum { KMS_USB_PROBING, KMS_USB_ACTIVE, KMS_USB_OFF, KMS_USB_LIMIT,
       KMS_USB_UNSUPPORTED, KMS_USB_ERROR, KMS_USB_REMOVING, KMS_USB_IGNORED, KMS_USB_STANDBY };
typedef struct { uint32_t max_us, at; } kms_usb_peak_t;
enum { KMS_USB_STREAM_DELAYS = 8 };
typedef struct { uint32_t at, gap_us, dispatch_us; } kms_usb_delay_t;
typedef struct {
    uint32_t completed, dropped, stale, expired, overflows, last_complete;
    kms_usb_peak_t callback, dispatch, poll_gap, decode;
    unsigned pending, peak;
    kms_usb_delay_t recent[KMS_USB_STREAM_DELAYS];
    unsigned recent_count;
} kms_usb_stream_status_t;
typedef struct {
    kms_usb_device_info_t devices[KMS_USB_DEVICES];
    kms_usb_interface_info_t interfaces[KMS_USB_INTERFACES];
    unsigned device_count, interface_count, controls, hubs, inputs;
    bool automatic;
    kms_usb_stream_status_t stream;
} kms_usb_inventory_t;

void kms_usb_devices_init(const kms_usb_input_callbacks_t *);
void kms_usb_devices_tick(void);
void kms_usb_devices_snapshot(kms_usb_inventory_t *);
// Cached application state only; no SDK calls on a stalled bus.
void kms_usb_devices_fault_snapshot(kms_usb_inventory_t *);
bool kms_usb_devices_select(unsigned address, unsigned number, bool enabled);
bool kms_usb_devices_matches(const kms_usb_device_info_t *);
bool kms_usb_devices_automatic(bool enabled);
void kms_usb_devices_rescan(void);
void kms_usb_devices_quiesce(bool quiescing);
bool kms_usb_devices_drained(void);
unsigned kms_usb_devices_active(void);
unsigned kms_usb_devices_healthy(void);
uint8_t kms_usb_device_address(const kms_usb_interface_t *);
uint8_t kms_usb_interface_number(const kms_usb_interface_t *);
const char *kms_usb_interface_state(unsigned state);
