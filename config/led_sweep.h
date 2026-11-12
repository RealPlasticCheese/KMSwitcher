#pragma once

// ESP32-S2 KEY V1.0 investigation only; no LED pin has been established.
// Exclude BOOT/strapping (0, 45, 46), USB (19, 20), absent GPIOs (22-25),
// and flash/PSRAM (26-32). The diagnostic disables UART before testing 43/44.
// Remove any additional pins used by peripherals on the board under test.
#define KMS_LED_SWEEP_GPIOS {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, \
                            33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44}
#define KMS_LED_SWEEP_DWELL_MS 1000u
#define KMS_LED_SWEEP_REPEATS 3u
#define KMS_LED_SWEEP_GAP_MS 500u
#define KMS_LED_SWEEP_START_MS 3000u
