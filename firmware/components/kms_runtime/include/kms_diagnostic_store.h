#pragma once
#include "kms_diagnostics.h"

void kms_diagnostics_init(uint32_t boot);
void kms_diagnostics_tick(void);
void kms_diagnostics_event(enum kms_event, unsigned slot, uint32_t detail);
// Capture on the input task; recover an RTC-saved fault after a guarded restart.
void kms_diagnostics_capture_event(kms_diag_record_t *, enum kms_event, unsigned slot, uint32_t detail);
void kms_diagnostics_recover_event(const kms_diag_record_t *);
void kms_diagnostics_input(const kms_state_t *previous, const kms_sample_t *, bool held);
void kms_diagnostics_held(bool held);
void kms_diagnostics_radio(bool transmit, bool retry, bool replay);
void kms_diagnostics_buffer(unsigned count);
void kms_diagnostics_status(void);
// Queue work to the low-priority diagnostic task; never block the input task.
void kms_diagnostics_dump(void);
// Optional minute ages are inclusive and scoped to the current boot.
bool kms_diagnostics_console(const char *line);
void kms_diagnostics_save(void);
