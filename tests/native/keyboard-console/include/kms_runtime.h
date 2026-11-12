#pragma once
#include "kms_text.h"
void kms_link_text_begin(void);
bool kms_link_text_supported(void);
bool kms_link_text_send(const char *, size_t);
void kms_link_text_cancel(void);
#include "kms_core.h"
uint32_t kms_now(void);
unsigned kms_link_selected(void);
bool kms_link_typing_available(void);
bool kms_link_typing_ready(void);
bool kms_link_keyboard_leds(uint8_t *);
void kms_link_clear_input(void);
void kms_link_emit(void *, const kms_state_t *, int32_t, int32_t, int32_t, int32_t);
