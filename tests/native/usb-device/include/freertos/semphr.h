#pragma once
#include <stdbool.h>
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define configASSERT(value) kms_fake_check((value) ? 0 : 1)
typedef struct { bool taken; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static unsigned kms_fake_nonblocking_rejections;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{ storage->taken = false; return storage; }
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned ticks)
{ if (mutex->taken) { if (!ticks) ++kms_fake_nonblocking_rejections; return 0; } mutex->taken = true; return pdTRUE; }
static inline int xSemaphoreGive(SemaphoreHandle_t mutex)
{ mutex->taken = false; return pdTRUE; }
