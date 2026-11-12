#pragma once
#include "queue.h"
QueueHandle_t xQueueCreateWithCaps(unsigned count, unsigned bytes, unsigned caps);
void vQueueDeleteWithCaps(QueueHandle_t queue);
