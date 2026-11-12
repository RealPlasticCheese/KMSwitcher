#pragma once

#include "esp_err.h"

// Call after installing the console UART driver. Input keeps its buffered RX path.
esp_err_t kms_console_output_init(void);
// Called by the console worker, never the input loop. Flush a partial last line.
void kms_console_output_flush(void);
