// Compile the production wait/wake bridge against a task notification adapter.
#include "../../firmware/components/kms_runtime/app.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, notifications, slept, mistakes;
static unsigned priority;
static bool arrival;
static int owner;

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &owner; }
void vTaskPrioritySet(TaskHandle_t task, unsigned value)
{ if (task != &owner) ++mistakes; priority = value; }
void xTaskNotifyGive(TaskHandle_t task)
{
    if (task != &owner) ++mistakes;
    ++notifications;
}
unsigned ulTaskNotifyTake(int clear, unsigned ticks)
{
    if (clear != pdTRUE || ticks != KMS_MAIN_POLL_MS) ++mistakes;
    // Model the atomic FreeRTOS wait: an arrival during the wait wakes it too.
    if (arrival) { arrival = false; kms_app_wake(); }
    unsigned taken = notifications;
    notifications = 0;
    if (!taken) slept += ticks;
    return taken;
}

__declspec(dllexport) int kms_run_app_tests(void)
{
    // An early USB callback before the owner binds is harmless.
    kms_app_wake(); CHECK(!notifications);
    kms_app_init();
    CHECK(priority == KMS_APP_TASK_PRIORITY && priority > 1 && priority < KMS_USB_TASK_PRIORITY);
    kms_app_wait(); CHECK(slept == KMS_MAIN_POLL_MS);
    // Work arriving before the wait cannot lose its wakeup.
    kms_app_wake(); kms_app_wait(); CHECK(slept == KMS_MAIN_POLL_MS && !notifications);
    // A burst is drained together, then periodic polling resumes.
    kms_app_wake(); kms_app_wake(); kms_app_wake();
    kms_app_wait(); CHECK(slept == KMS_MAIN_POLL_MS && !notifications);
    kms_app_wait(); CHECK(slept == 2 * KMS_MAIN_POLL_MS);
    arrival = true; kms_app_wait(); CHECK(slept == 2 * KMS_MAIN_POLL_MS && !notifications);
    CHECK(!mistakes);
    return 0;
}
__declspec(dllexport) unsigned kms_app_test_count(void) { return checks; }
