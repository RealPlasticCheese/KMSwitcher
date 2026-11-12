#pragma once
#include <stdint.h>
#include <stdbool.h>
#define INT8_MAX 127
#define INT16_MAX 32767
#define TUSB_DESC_DEVICE 1
#define TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP 0x20
#define CFG_TUD_ENDPOINT0_SIZE 64
#define HID_PROTOCOL_BOOT 0
#define HID_PROTOCOL_REPORT 1
#define HID_REPORT_TYPE_INPUT 1
#define HID_REPORT_TYPE_OUTPUT 2
#define HID_REPORT_TYPE_FEATURE 3
#define TINYUSB_EVENT_ATTACHED 1
#define TINYUSB_EVENT_DETACHED 2
#define TUD_CONFIG_DESC_LEN 9
#define TUD_HID_DESC_LEN 25
#define TUD_CONFIG_DESCRIPTOR(config, interfaces, string, length, attributes, power) (attributes)
#define TUD_HID_DESCRIPTOR(...) 0
typedef unsigned hid_report_type_t;
typedef struct {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} tusb_desc_device_t;
typedef struct { unsigned id; } tinyusb_event_t;
typedef struct {
    void (*event_cb)(tinyusb_event_t *, void *);
    struct { unsigned size, priority; } task;
    struct { const tusb_desc_device_t *device; const uint8_t *full_speed_config; const char **string; unsigned string_count; } descriptor;
} tinyusb_config_t;
bool tud_mounted(void);
bool tud_suspended(void);
bool tud_remote_wakeup(void);
uint8_t tud_hid_n_get_protocol(unsigned);
bool tud_hid_n_ready(unsigned);
bool tud_hid_n_report(unsigned, unsigned, const void *, unsigned);
void tud_hid_report_complete_cb(uint8_t, const uint8_t *, uint16_t);
int tinyusb_driver_install(const tinyusb_config_t *);
