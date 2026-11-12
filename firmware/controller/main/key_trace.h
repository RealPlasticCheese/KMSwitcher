#pragma once
#include "kms_key_trace.h"

void kms_controller_key_trace_init(void);
void kms_controller_key_trace(void *, const kms_key_trace_t *);
void kms_controller_router_key_trace(void *, const kms_key_trace_t *);
bool kms_controller_key_trace_console(const char *);
void kms_controller_key_trace_drop(enum kms_key_trace_reason, unsigned source,
    unsigned slot, const kms_state_t *before, const kms_state_t *after);
