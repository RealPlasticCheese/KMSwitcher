#include "console_baud.h"
#include "console_output.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BASE_BAUD 115200u
#define LEASE_US 10000000LL

typedef struct {
    uint32_t id, rate;
    bool valid, set;
} baud_request_t;

static QueueHandle_t requests;
static uint32_t boot;
static const uint32_t rates[] = {
    BASE_BAUD, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000, 4000000
};

static bool supported(uint32_t rate)
{
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
        if (rates[i] == rate) return true;
    return false;
}

static void reply(const baud_request_t *request, const char *status, uint32_t rate)
{
    printf("KMSBAUD %" PRIu32 " %s %" PRIu32 " %" PRIu32 "\n", request->id, status, rate, boot);
    fflush(stdout);
}

static void worker(void *context)
{
    (void)context;
    uint32_t rate = BASE_BAUD;
    int64_t lease_until = 0;
    baud_request_t request;
    while (true) {
        // Also drain output without a newline, including a final single byte.
        kms_console_output_flush();
        if (xQueueReceive(requests, &request, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (!request.valid) { reply(&request, "INVALID", rate); continue; }
            if (request.set && request.rate != rate) {
                reply(&request, "SWITCH", request.rate);
                // Drain the acknowledgement at the old rate, away from the input task.
                uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
                vTaskDelay(pdMS_TO_TICKS(200));
                if (uart_set_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, request.rate) != ESP_OK) {
                    reply(&request, "ERROR", rate);
                    continue;
                }
                rate = request.rate;
            } else reply(&request, "OK", rate);
            lease_until = esp_timer_get_time() + LEASE_US;
        }
        if (rate != BASE_BAUD && esp_timer_get_time() >= lease_until) {
            uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
            if (uart_set_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, BASE_BAUD) == ESP_OK) rate = BASE_BAUD;
        }
    }
}

void kms_console_baud_init(void)
{
    // Use the 80 MHz UART source for the supported high console rates. The boot
    // console may use XTAL instead, so do not inherit its clock configuration.
    const uart_config_t configuration = {
        .baud_rate = BASE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    if (uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &configuration) != ESP_OK) {
        ESP_LOGE("console", "UART clock configuration failed; speed negotiation disabled");
        return;
    }
    // A full management line can arrive between input-loop polls at 4 Mbaud.
    // Interrupt-driven RX keeps it in RAM rather than overflowing the hardware FIFO.
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM) &&
        uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 4096, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE("console", "UART receive buffer unavailable; speed negotiation disabled");
        return;
    }
    // The default 120-byte watermark leaves only 20 us of FIFO headroom at
    // 4 Mbaud. Drain earlier so USB/radio interrupt latency does not lose the
    // beginning of a full management command.
    if (uart_set_rx_full_threshold(CONFIG_ESP_CONSOLE_UART_NUM, 32) != ESP_OK) {
        ESP_LOGE("console", "UART receive threshold configuration failed");
        return;
    }
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    boot = esp_random();
    requests = xQueueCreate(4, sizeof(baud_request_t));
    if (!requests) return;
    if (xTaskCreate(worker, "kms-baud", 2560, NULL, 1, NULL) != pdPASS) {
        vQueueDelete(requests);
        requests = NULL;
        return;
    }
    if (kms_console_output_init() != ESP_OK)
        ESP_LOGW("console", "Block output unavailable; using the standard UART console");
}

bool kms_console_baud_command(const char *line)
{
    if (strncmp(line, "baud", 4) || (line[4] && line[4] != ' ')) return false;
    baud_request_t request = {0};
    unsigned long id = 0, rate = 0;
    int end = 0;
    if (sscanf(line, "baud get %lu%n", &id, &end) == 1 && !line[end] && id) {
        request.valid = true;
    } else if (sscanf(line, "baud set %lu %lu%n", &id, &rate, &end) == 2 && !line[end] && id && supported(rate)) {
        request.valid = true;
        request.set = true;
        request.rate = rate;
    }
    request.id = id;
    if (!requests || xQueueSend(requests, &request, 0) != pdTRUE) reply(&request, "BUSY", BASE_BAUD);
    return true;
}
