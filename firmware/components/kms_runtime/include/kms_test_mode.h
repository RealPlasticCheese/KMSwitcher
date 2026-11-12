#pragma once
#include <stdbool.h>
#include <stdint.h>

// The master persists the session boundary, never individual trace records.
void kms_test_mode_init(bool master);
bool kms_test_mode_active(void);
bool kms_test_mode_set(bool active);
// History workers share this budget; ordinary operation is unrestricted.
bool kms_test_mode_history_allowed(uint32_t now);
