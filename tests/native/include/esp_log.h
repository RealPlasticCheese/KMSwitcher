#pragma once
void kms_fake_log(const char *, const char *, ...);
void kms_fake_check(int);
#define ESP_LOGI(...) kms_fake_log(__VA_ARGS__)
#define ESP_LOGW(...) kms_fake_log(__VA_ARGS__)
#define ESP_LOGE(...) kms_fake_log(__VA_ARGS__)
#define ESP_ERROR_CHECK(e) kms_fake_check(e)
