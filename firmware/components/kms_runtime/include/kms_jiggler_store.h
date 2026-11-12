#pragma once
#include "kms_jiggler.h"

// Load before the worker starts. Updates copy configuration only; NVS runs on the worker.
void kms_jiggler_store_load(kms_jiggler_t *, uint32_t now);
void kms_jiggler_store_update(const kms_jiggler_t *);
void kms_jiggler_store_maintain(void);
bool kms_jiggler_store_flush(void);
