#pragma once
#include <stddef.h>
#include <stdint.h>
enum { ESP_OK, ESP_FAIL, WIFI_PHY_MODE_11B, WIFI_PHY_MODE_11G,
       WIFI_PHY_RATE_1M_L, WIFI_PHY_RATE_6M, WIFI_PHY_RATE_12M };
typedef struct { unsigned phymode, rate; } esp_now_rate_config_t;
int esp_now_set_peer_rate_config(const uint8_t *, esp_now_rate_config_t *);
