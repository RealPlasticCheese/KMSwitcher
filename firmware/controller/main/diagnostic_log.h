#pragma once
#include "kms_core.h"
void kms_diagnostic_log_init(void);
bool kms_diagnostic_log_console(const char *command);
bool kms_diagnostic_log_type(void *ctx);
void kms_diagnostic_log_stop(void *ctx);
void kms_diagnostic_log_tick(kms_router_t *router);
bool kms_diagnostic_console_input(void *ctx, const kms_state_t *previous, const kms_state_t *state);
bool kms_diagnostic_console_command(char *line, unsigned capacity);
