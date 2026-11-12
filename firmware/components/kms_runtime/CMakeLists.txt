idf_component_register(SRCS "app.c" "memory.c" "desktop_runtime.c" "link.c" "radio_channel.c" "pair_crypto.c" "board.c" "diagnostic_store.c" "build_identity.c" "slave_diagnostics.c" "ota.c" "remote.c" "test_trace.c" "test_mode.c"
                       INCLUDE_DIRS "include" "../../../config"
                       REQUIRES kms_core esp_wifi esp_event nvs_flash
                                esp_timer esp_driver_gpio esp_hw_support heap mbedtls app_update bootloader_support esp_app_format)
set(kms_generated "${CMAKE_BINARY_DIR}/generated")
target_sources(${COMPONENT_LIB} PRIVATE "receiver_cache.c")
target_sources(${COMPONENT_LIB} PRIVATE "jiggler_store.c" "stream_runtime.c" "stream_radio.c")
add_custom_target(kms_build_stamp
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/build-metadata.py" stamp
        --root "${CMAKE_CURRENT_LIST_DIR}/../../.."
        --header "${kms_generated}/kms_build_identity.h"
    BYPRODUCTS "${kms_generated}/kms_build_identity.h" "${kms_generated}/kms_build_identity.json"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} kms_build_stamp)
target_include_directories(${COMPONENT_LIB} PRIVATE "${kms_generated}")
if(IDF_TARGET STREQUAL "esp32s3")
    target_sources(${COMPONENT_LIB} PRIVATE "channel_recovery.c")
    idf_component_optional_requires(PRIVATE espressif__led_strip)
endif()
