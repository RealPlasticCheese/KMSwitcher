#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, USB_SETUP_PACKET_SIZE = 8,
       USB_TRANSFER_STATUS_COMPLETED = 0, USB_TRANSFER_STATUS_ERROR = 1,
       USB_HOST_CLIENT_EVENT_DEV_GONE = 1,
       USB_BM_REQUEST_TYPE_DIR_OUT = 0, USB_BM_REQUEST_TYPE_TYPE_CLASS = 0x20,
       USB_BM_REQUEST_TYPE_RECIP_INTERFACE = 1 };
typedef void *usb_device_handle_t;
typedef void *usb_host_client_handle_t;
typedef struct {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} usb_setup_packet_t;
typedef struct usb_transfer_s {
    uint8_t *data_buffer;
    size_t data_buffer_size;
    int num_bytes, actual_num_bytes, status;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    void (*callback)(struct usb_transfer_s *);
    void *context;
} usb_transfer_t;
typedef struct { int event; struct { usb_device_handle_t dev_hdl; } dev_gone; } usb_host_client_event_msg_t;
typedef struct {
    bool is_synchronous;
    int max_num_event_msg;
    struct { void (*client_event_callback)(const usb_host_client_event_msg_t *, void *); void *callback_arg; } async;
} usb_host_client_config_t;
esp_err_t usb_host_client_register(const usb_host_client_config_t *, usb_host_client_handle_t *);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, int);
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t *);
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t);
esp_err_t usb_host_transfer_alloc(size_t, int, usb_transfer_t **);
esp_err_t usb_host_transfer_free(usb_transfer_t *);
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t, usb_transfer_t *);
