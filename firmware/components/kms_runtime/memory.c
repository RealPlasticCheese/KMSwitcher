#include "kms_memory.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdlib.h>

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#endif

void *kms_memory_calloc(size_t count, size_t bytes)
{
    if (!count || !bytes || count > SIZE_MAX / bytes) return NULL;
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    void *buffer = heap_caps_calloc(count, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer) return buffer;
#endif
    return calloc(count, bytes);
}

QueueHandle_t kms_memory_queue_create(unsigned count, unsigned bytes)
{
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    QueueHandle_t queue = xQueueCreateWithCaps(count, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return queue ? queue : xQueueCreateWithCaps(count, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xQueueCreate(count, bytes);
#endif
}

void kms_memory_queue_delete(QueueHandle_t queue)
{
    if (!queue) return;
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    vQueueDeleteWithCaps(queue);
#else
    vQueueDelete(queue);
#endif
}
