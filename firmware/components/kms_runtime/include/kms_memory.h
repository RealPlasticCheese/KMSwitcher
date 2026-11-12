#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Task-only data: prefer S3 PSRAM, retain internal allocation as fallback.
// Free successful allocations with free(). Zero-sized requests return NULL.
void *kms_memory_calloc(size_t count, size_t bytes);

// Queues live in PSRAM on the S3 when available. Always pair creation/deletion
// through these functions, including startup-error cleanup after fallback.
QueueHandle_t kms_memory_queue_create(unsigned count, unsigned bytes);
void kms_memory_queue_delete(QueueHandle_t queue);
