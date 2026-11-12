idf_component_register(SRCS "main.c" "stream.c" "../../hid-generator/main/pattern.c"
    INCLUDE_DIRS "." "../../hid-generator/main"
    REQUIRES espressif__usb esp_hw_support esp_timer esp_app_format soc)
idf_component_get_property(usb_version espressif__usb COMPONENT_VERSION)
if(NOT usb_version VERSION_EQUAL "1.6.0")
    message(FATAL_ERROR "USB isolation must use the pinned espressif/usb 1.6.0 stack")
endif()

# Compile a generated overlay rather than editing the component-manager cache.
option(KMS_ISO_DRIVER_TRACE "Observe HCD/DMA lifecycle in the spare-host reproducer" ON)
if(KMS_ISO_DRIVER_TRACE)
    if(NOT IDF_TARGET STREQUAL "esp32s3")
        message(FATAL_ERROR "Isolation driver trace supports only ESP32-S3")
    endif()
    idf_component_get_property(usb_dir espressif__usb COMPONENT_DIR)
    idf_component_get_property(usb_lib espressif__usb COMPONENT_LIB)
    set(trace_dir "${CMAKE_CURRENT_LIST_DIR}/..")
    set(hcd_overlay "${CMAKE_CURRENT_BINARY_DIR}/hcd_dwc_traced.c")
    execute_process(COMMAND "${PYTHON}" "${trace_dir}/instrument_hcd.py"
        "${usb_dir}/src/hcd_dwc.c" "${hcd_overlay}" COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${usb_dir}/src/hcd_dwc.c" "${trace_dir}/instrument_hcd.py")
    get_target_property(usb_sources ${usb_lib} SOURCES)
    list(LENGTH usb_sources original_source_count)
    list(FILTER usb_sources EXCLUDE REGEX "(^|/)hcd_dwc\\.c$")
    list(LENGTH usb_sources remaining_source_count)
    math(EXPR removed_source_count "${original_source_count} - ${remaining_source_count}")
    if(NOT removed_source_count EQUAL 1)
        message(FATAL_ERROR "Expected exactly one upstream HCD translation unit")
    endif()
    set_property(TARGET ${usb_lib} PROPERTY SOURCES "${usb_sources}")
    target_sources(${usb_lib} PRIVATE "${hcd_overlay}"
        "${CMAKE_CURRENT_LIST_DIR}/trace_store.c" "${CMAKE_CURRENT_LIST_DIR}/trace_port.c"
        "${CMAKE_CURRENT_LIST_DIR}/frame_guard.c")
    target_include_directories(${usb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${trace_dir}")
    target_link_libraries(${usb_lib} PRIVATE idf::esp_timer)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ISO_DRIVER_TRACE=1)
    idf_component_get_property(hal_dir esp_hal_usb COMPONENT_DIR)
    idf_component_get_property(hal_lib esp_hal_usb COMPONENT_LIB)
    set(hal_overlay "${CMAKE_CURRENT_BINARY_DIR}/usb_dwc_hal_traced.c")
    execute_process(COMMAND "${PYTHON}" "${trace_dir}/instrument_hal.py"
        "${hal_dir}/usb_dwc_hal.c" "${hal_overlay}" COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${hal_dir}/usb_dwc_hal.c" "${trace_dir}/instrument_hal.py"
        "${trace_dir}/../../firmware/usb_frame_guard/instrument_hal.py")
    get_target_property(hal_sources ${hal_lib} SOURCES)
    list(LENGTH hal_sources hal_before)
    list(FILTER hal_sources EXCLUDE REGEX "(^|/)usb_dwc_hal\\.c$")
    list(LENGTH hal_sources hal_after)
    math(EXPR hal_removed "${hal_before}-${hal_after}")
    if(NOT hal_removed EQUAL 1)
        message(FATAL_ERROR "Expected exactly one USB HAL translation unit")
    endif()
    set_property(TARGET ${hal_lib} PROPERTY SOURCES "${hal_sources}")
    target_sources(${hal_lib} PRIVATE "${hal_overlay}")
    target_include_directories(${hal_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
endif()
