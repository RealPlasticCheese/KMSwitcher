#pragma once
#include "nvs.h"
typedef struct { unsigned schan, nchan; } wifi_country_t;
enum { WIFI_SECOND_CHAN_NONE };
esp_err_t esp_wifi_get_country(wifi_country_t *);
esp_err_t esp_wifi_set_channel(uint8_t, int);
