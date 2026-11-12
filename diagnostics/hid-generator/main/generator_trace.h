#pragma once
#include <stdint.h>
void kms_gen_trace_irq_enter(uint32_t status);
void kms_gen_trace_irq_exit(void);
void kms_gen_trace_arm(const void *buffer, uint32_t bytes, uint32_t packet);
void kms_gen_trace_mark(uint32_t kind, uint32_t value);
void kms_gen_trace_start(uint32_t run);
void kms_gen_trace_tick(void);
void kms_gen_trace_freeze(void);
void kms_gen_trace_print(uint32_t token, const char *identity);
