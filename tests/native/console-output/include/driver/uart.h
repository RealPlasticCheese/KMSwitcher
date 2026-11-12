#pragma once
#include <stddef.h>
#define CONFIG_ESP_CONSOLE_UART_NUM 0
int uart_write_bytes(int, const void *, size_t);
