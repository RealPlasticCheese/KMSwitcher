#include "kms_runtime.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t application;

void kms_app_init(void)
{
    application = xTaskGetCurrentTaskHandle();
    vTaskPrioritySet(application, KMS_APP_TASK_PRIORITY);
}

void kms_app_wake(void)
{
    // Wi-Fi and TinyUSB callbacks run on tasks, never in interrupt context.
    // A notification also survives an arrival just before the owner waits.
    if (application) xTaskNotifyGive(application);
}

void kms_app_wait(void)
{
    // Keep periodic lease/button/console polling when there is no input.
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(KMS_MAIN_POLL_MS));
}
