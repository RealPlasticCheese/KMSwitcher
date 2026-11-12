#pragma once
#include "../../include/nvs.h"
esp_err_t nvs_open_from_partition(const char *, const char *, int, nvs_handle_t *);
