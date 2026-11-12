#pragma once
#include <stdarg.h>
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t);
void kms_console_fake_log(const char *, const char *, ...);
#define ESP_LOGI(...) kms_console_fake_log(__VA_ARGS__)
#define ESP_LOGW(...) kms_console_fake_log(__VA_ARGS__)
