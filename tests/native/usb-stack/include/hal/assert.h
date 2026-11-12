#pragma once
void kms_sdk_assert(void);
#define HAL_ASSERT(c) do { if (!(c)) kms_sdk_assert(); } while (0)
