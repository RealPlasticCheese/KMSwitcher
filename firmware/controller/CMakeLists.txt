cmake_minimum_required(VERSION 3.22)
include("../build_identity.cmake")
set(EXTRA_COMPONENT_DIRS "../components")
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "../../config/sdkconfig.flash.esp32s3;../../config/sdkconfig.common;../../config/sdkconfig.controller")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(kmswitcher_controller)
if(CONFIG_KMS_TEST_SUPPORT)
    idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_HID_EP_BUFSIZE=256" APPEND)
endif()
kms_add_manifest(controller 9)
if(KMS_RECEIVER_BUILD)
    add_custom_target(kms_receiver_cache ALL
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../scripts/build-receiver-cache.py"
            --controller "${CMAKE_BINARY_DIR}" --receiver "${KMS_RECEIVER_BUILD}"
        BYPRODUCTS "${CMAKE_BINARY_DIR}/receiver-cache.bin" "${CMAKE_BINARY_DIR}/receiver-cache-empty.bin"
        DEPENDS kms_firmware_manifest VERBATIM)
    esptool_py_flash_target_image(flash receiver_cache "0x960000" "${CMAKE_BINARY_DIR}/receiver-cache.bin")
    esptool_py_flash_target_image(flash receiver_cache_empty "0xb60000" "${CMAKE_BINARY_DIR}/receiver-cache-empty.bin")
    add_dependencies(flash kms_receiver_cache)
endif()
