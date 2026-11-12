idf_component_register(SRCS "main.c" "../../board-check/main/usb_console.c"
                       INCLUDE_DIRS "../../../config"
                       REQUIRES esp_driver_gpio freertos log espressif__esp_tinyusb)
