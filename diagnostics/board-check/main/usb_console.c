#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "kms_settings.h"

void kms_diagnostic_console_init(void)
{
    tinyusb_config_t c = TINYUSB_DEFAULT_CONFIG();
    c.task.size = KMS_USB_TASK_STACK; c.task.priority = KMS_USB_TASK_PRIORITY;
    ESP_ERROR_CHECK(tinyusb_driver_install(&c));
    tinyusb_config_cdcacm_t acm = {.cdc_port = TINYUSB_CDC_ACM_0};
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm));
    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));
}
