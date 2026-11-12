# Human release numbers are unnecessary: source revision is the app version.
execute_process(COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git rev-parse --git-path HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    OUTPUT_VARIABLE kms_git_head OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
get_filename_component(kms_git_head "${kms_git_head}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
execute_process(COMMAND git symbolic-ref -q HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    OUTPUT_VARIABLE kms_git_branch OUTPUT_STRIP_TRAILING_WHITESPACE)
if(kms_git_branch)
    execute_process(COMMAND git rev-parse --git-path "${kms_git_branch}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
        OUTPUT_VARIABLE kms_git_ref OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    get_filename_component(kms_git_ref "${kms_git_ref}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${kms_git_ref}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${kms_git_head}")

function(kms_add_manifest role chip)
    add_custom_target(kms_firmware_manifest ALL
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/build-metadata.py" manifest
            --binary "${CMAKE_BINARY_DIR}/kmswitcher_${role}.bin"
            --header "${CMAKE_BINARY_DIR}/generated/kms_build_identity.h"
            --role "${role}" --chip "${chip}"
        DEPENDS gen_project_binary VERBATIM)
    if(TARGET flash)
        add_dependencies(flash kms_firmware_manifest)
    endif()
endfunction()
