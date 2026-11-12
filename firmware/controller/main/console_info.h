#pragma once

void kms_console_info_init(void);
// Queue informational output without waiting for the UART in the input task.
void kms_console_help(void);
void kms_console_about(void);
