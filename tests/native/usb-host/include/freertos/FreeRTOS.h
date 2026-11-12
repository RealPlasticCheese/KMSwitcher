#pragma once
enum { pdPASS = 1, pdTRUE = 1, pdFALSE = 0 };
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(value) (value)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL_SAFE(lock) portENTER_CRITICAL(lock)
#define portEXIT_CRITICAL_SAFE(lock) portEXIT_CRITICAL(lock)
void kms_host_assert(int);
#define configASSERT(value) kms_host_assert(!!(value))
