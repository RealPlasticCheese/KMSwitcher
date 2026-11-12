idf_component_register(SRCS "bringup.c" INCLUDE_DIRS "../../../config"
                       REQUIRES esp_system esp_hw_support freertos log)
target_compile_definitions(${COMPONENT_LIB} PRIVATE KMS_ROLE="diagnostic")
