#pragma once
enum { ESP_RST_POWERON = 1, ESP_RST_SW = 3 };
int esp_reset_reason(void);
void esp_restart(void);
