#pragma once
typedef struct { const char *version, *idf_ver; } esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void)
{ static esp_app_desc_t app = {"test", "v6.1"}; return &app; }
