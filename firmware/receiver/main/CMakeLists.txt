idf_component_register(SRCS "main.c" "usb_device.c"
                       INCLUDE_DIRS "."
                       REQUIRES kms_runtime espressif__esp_tinyusb esp_hw_support)
