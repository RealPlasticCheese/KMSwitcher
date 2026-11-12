#pragma once
#define pdMS_TO_TICKS(n) (n)
static inline void vTaskDelay(unsigned n) { (void)n; }
