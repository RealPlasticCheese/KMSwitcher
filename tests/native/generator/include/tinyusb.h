#pragma once
#include <stdbool.h>
#include <stdint.h>
#define ESP_ERROR_CHECK(x) ((void)(x))
#define CFG_TUD_ENDPOINT0_SIZE 64
#define TUSB_DESC_DEVICE 1
#define HID_ITF_PROTOCOL_NONE 0
#define TUD_CONFIG_DESC_LEN 9
#define TUD_HID_DESC_LEN 9
#define TUD_CONFIG_DESCRIPTOR(...) 0
#define TUD_HID_DESCRIPTOR(...) 0
enum { TINYUSB_EVENT_ATTACHED, TINYUSB_EVENT_DETACHED, TINYUSB_EVENT_SUSPENDED, TINYUSB_EVENT_RESUMED };
typedef struct { int id; } tinyusb_event_t;
typedef enum { HID_REPORT_TYPE_INPUT = 1, HID_REPORT_TYPE_OUTPUT } hid_report_type_t;
typedef struct {
    unsigned bLength, bDescriptorType, bcdUSB, bMaxPacketSize0;
    unsigned idVendor, idProduct, bcdDevice, iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} tusb_desc_device_t;
typedef struct {
    void (*event_cb)(tinyusb_event_t *, void *);
    struct {
        const tusb_desc_device_t *device;
        const uint8_t *full_speed_config;
        const char **string;
        unsigned string_count;
    } descriptor;
} tinyusb_config_t;
static inline int tinyusb_driver_install(const tinyusb_config_t *c) { (void)c; return 0; }
bool tud_mounted(void);
bool tud_suspended(void);
bool tud_hid_n_ready(uint8_t instance);
bool tud_hid_n_report(uint8_t instance, uint8_t id, const void *data, uint16_t n);
