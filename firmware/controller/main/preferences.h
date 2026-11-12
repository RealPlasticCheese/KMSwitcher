#pragma once
#include "kms_runtime.h"
void kms_preferences_init(uint32_t revision_seed);
const kms_user_preferences_t *kms_preferences_get(void);
uint32_t kms_preferences_revision(void);
bool kms_preferences_set(const kms_user_preferences_t *);
bool kms_preferences_console(const char *line);
kms_trigger_t kms_preferences_trigger(void);
bool kms_preferences_save_trigger(void *ctx, kms_trigger_t trigger);
bool kms_preferences_factory_reset(void);
bool kms_preferences_save_boot_slot(void *ctx, unsigned slot);
