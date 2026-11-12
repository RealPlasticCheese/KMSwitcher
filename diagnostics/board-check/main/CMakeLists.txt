idf_component_register(SRCS "main.c" "../../../firmware/components/kms_runtime/board.c"
                             "../../../firmware/components/kms_runtime/pair_crypto.c"
                             "../../../firmware/components/kms_core/controls.c"
                             "../../../firmware/components/kms_core/wire.c"
                       INCLUDE_DIRS "../../../config" "../../../firmware/components/kms_core/include"
                                    "../../../firmware/components/kms_runtime/include"
                                    "../../../firmware/components/kms_runtime"
                       REQUIRES esp_system esp_hw_support esp_timer esp_driver_gpio freertos log mbedtls espressif__led_strip)
if(IDF_TARGET STREQUAL "esp32s2")
    target_sources(${COMPONENT_LIB} PRIVATE "usb_console.c")
    idf_component_optional_requires(PRIVATE espressif__esp_tinyusb)
endif()
