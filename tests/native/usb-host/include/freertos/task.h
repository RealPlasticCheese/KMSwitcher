#pragma once
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskPrioritySet(TaskHandle_t, unsigned);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(int clear, unsigned ticks);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *, unsigned, void *, int);
void vTaskDelay(unsigned ticks);
void vTaskSuspend(void *task);
