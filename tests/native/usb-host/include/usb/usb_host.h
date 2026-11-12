#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_INVALID_RESPONSE, ESP_ERR_NOT_SUPPORTED,
       ESP_INTR_FLAG_LEVEL1 = 2, USB_SETUP_PACKET_SIZE = 8,
       USB_HOST_CLIENT_EVENT_NEW_DEV = 1, USB_HOST_CLIENT_EVENT_DEV_GONE = 2,
       USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS = 1,
       USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_ERROR, USB_TRANSFER_STATUS_CANCELED,
       USB_TRANSFER_STATUS_NO_DEVICE, USB_TRANSFER_TYPE_INTR = 3,
       USB_B_DESCRIPTOR_TYPE_INTERFACE = 4, USB_B_DESCRIPTOR_TYPE_ENDPOINT = 5, USB_B_REQUEST_GET_DESCRIPTOR = 6 };
typedef void *usb_device_handle_t;
typedef void *usb_host_client_handle_t;
#pragma pack(push, 1)
typedef struct { uint8_t bLength, bDescriptorType; uint16_t bcdUSB; uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice; uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations; } usb_device_desc_t;
typedef struct { uint8_t bLength, bDescriptorType; uint16_t wTotalLength; uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower; } usb_config_desc_t;
typedef struct { uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface; } usb_intf_desc_t;
typedef struct { uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes; uint16_t wMaxPacketSize; uint8_t bInterval; } usb_ep_desc_t;
#ifdef _MSC_VER
#pragma warning(push)
// VS 2022 warns about this standard C flexible member; retain the SDK's ABI.
#pragma warning(disable: 4200)
#endif
typedef struct { uint8_t bLength, bDescriptorType; uint16_t wData[]; } usb_str_desc_t;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
typedef struct { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; } usb_setup_packet_t;
#pragma pack(pop)
#define USB_EP_DESC_GET_EP_DIR(ep) (((ep)->bEndpointAddress & 128) != 0)
#define USB_EP_DESC_GET_XFERTYPE(ep) ((ep)->bmAttributes & 3)
#define USB_EP_DESC_GET_MPS(ep) ((ep)->wMaxPacketSize & 2047)
typedef struct { struct { usb_device_handle_t dev_hdl; uint8_t port_num; } parent;
    int speed; uint8_t dev_addr; const usb_str_desc_t *str_desc_manufacturer, *str_desc_product, *str_desc_serial_num; } usb_device_info_t;
typedef struct usb_transfer_s { uint8_t *data_buffer; size_t data_buffer_size; int num_bytes, actual_num_bytes, status;
    usb_device_handle_t device_handle; uint8_t bEndpointAddress; void (*callback)(struct usb_transfer_s *); void *context; } usb_transfer_t;
typedef struct { int event; union { struct { uint8_t address; } new_dev; struct { usb_device_handle_t dev_hdl; } dev_gone; }; } usb_host_client_event_msg_t;
typedef struct { bool is_synchronous; int max_num_event_msg;
    struct { void (*client_event_callback)(const usb_host_client_event_msg_t *, void *); void *callback_arg; } async; } usb_host_client_config_t;
typedef struct { int intr_flags; } usb_host_config_t;
typedef struct { int num_devices, num_clients; bool root_port_suspended; } usb_host_lib_info_t;
const char *esp_err_to_name(esp_err_t);
esp_err_t usb_host_install(const usb_host_config_t *);
esp_err_t usb_host_client_register(const usb_host_client_config_t *, usb_host_client_handle_t *);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, int);
esp_err_t usb_host_lib_handle_events(int, uint32_t *);
esp_err_t usb_host_device_free_all(void);
esp_err_t usb_host_lib_info(usb_host_lib_info_t *);
esp_err_t usb_host_lib_set_root_port_power(bool);
esp_err_t usb_host_device_addr_list_fill(int, uint8_t *, int *);
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t *);
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t);
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t, const usb_device_desc_t **);
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t **);
esp_err_t usb_host_device_info(usb_device_handle_t, usb_device_info_t *);
esp_err_t usb_host_transfer_alloc(size_t, int, usb_transfer_t **);
esp_err_t usb_host_transfer_free(usb_transfer_t *);
esp_err_t usb_host_transfer_submit(usb_transfer_t *);
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t, usb_transfer_t *);
esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t);
esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t);
esp_err_t usb_host_endpoint_halt(usb_device_handle_t, uint8_t);
esp_err_t usb_host_endpoint_flush(usb_device_handle_t, uint8_t);
