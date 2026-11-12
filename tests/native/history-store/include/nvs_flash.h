#pragma once
#include "nvs.h"
esp_err_t nvs_flash_init_partition(const char *);
esp_err_t nvs_open_from_partition(const char *, const char *, int, nvs_handle_t *);
