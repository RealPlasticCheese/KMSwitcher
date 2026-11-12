idf_component_register(SRCS "main.c" "diagnostic_log.c" "console_info.c" "console_baud.c" "console_output.c" "usb_input.c" "usb_devices.c" "usb_console.c" "usb_preferences.c" "usb_leds.c" "usb_health.c" "usb_bus.c" "usb_fault.c" "usb_driver_trace.c" "usb_memory.c" "usb_recovery.c" "preferences.c" "preferences_console.c"
                       INCLUDE_DIRS "."
                       REQUIRES kms_runtime espressif__usb_host_hid nvs_flash esp_app_format esp_timer soc
                       PRIV_REQUIRES esp_driver_uart esp_psram esp_hal_usb espressif__usb vfs)

# Private URB layout is pinned by dependencies.lock (espressif/usb 1.6.0).
target_sources(${COMPONENT_LIB} PRIVATE "key_trace.c")
target_sources(${COMPONENT_LIB} PRIVATE "device_controls.c")

idf_component_get_property(usb_version espressif__usb COMPONENT_VERSION)
if(NOT usb_version VERSION_EQUAL "1.6.0")
    message(FATAL_ERROR "Review USB trace hooks before changing espressif/usb 1.6.0")
endif()
idf_component_get_property(usb_dir espressif__usb COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${usb_dir}/private_include")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../usb_frame_guard")
include("../../usb_frame_guard/guard_hal.cmake")
kms_usb_frame_guard_apply()
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=usb_dwc_hal_chan_activate"
    "-Wl,--wrap=usb_dwc_hal_chan_free"
    "-Wl,--wrap=usb_dwc_hal_chan_decode_intr"
    "-Wl,--wrap=usb_dwc_hal_decode_intr"
    "-Wl,--wrap=hcd_urb_dequeue"
    "-Wl,--wrap=printf"
    "-Wl,--wrap=puts"
    "-Wl,--wrap=fputs"
    "-Wl,--wrap=putchar"
    "-Wl,--wrap=fwrite")
