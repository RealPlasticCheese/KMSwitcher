#pragma once
// YD-style ESP32-S3 N16R8: manufacturer documents GPIO48 for the WS2812 RGB.
#define KMS_S3_BUTTON_GPIO 0
#define KMS_S3_BUTTON_ACTIVE_LOW 1
#define KMS_S3_RGB_GPIO 48
#define KMS_S3_RGB_MODEL LED_MODEL_WS2812
#define KMS_S3_RGB_FORMAT LED_STRIP_COLOR_COMPONENT_FMT_GRB
// ESP32-S2 KEY V1.0: BOOT behaviour observed, LED GPIO not yet established.
#define KMS_S2_BUTTON_GPIO 0
#define KMS_S2_BUTTON_ACTIVE_LOW 1
#define KMS_S2_LED_GPIO (-1) // Leave disabled until the exact LED wiring is verified.
#define KMS_S2_LED_ACTIVE_LOW 0
// Never drive USB VBUS from an arbitrary GPIO. Host power is a board/hub wiring choice.
