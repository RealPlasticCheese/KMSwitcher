cmake_minimum_required(VERSION 3.22)
include("../build_identity.cmake")
set(EXTRA_COMPONENT_DIRS "../components")
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "../../config/sdkconfig.flash.esp32s2;../../config/sdkconfig.common;../../config/sdkconfig.receiver")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(kmswitcher_receiver)
# Desktop feature reports require 64 payload bytes plus their report ID even
# when optional trace/test controls are excluded from a custom build.
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_HID_EP_BUFSIZE=256" APPEND)
kms_add_manifest(receiver 2)
