#pragma once
#include "kms_core.h"
#include "kms_diagnostics.h"
#include "kms_desktop_runtime.h"
void kms_usb_input_init(kms_router_t *);
void kms_usb_input_tick(void);
void kms_usb_input_status(void);
void kms_usb_input_leds_enabled(bool enabled);
bool kms_usb_input_command(const char *line);
bool kms_usb_input_desktop(unsigned source, uint32_t boot, const kms_desktop_message_t *);
void kms_usb_input_desktop_replies(void);

// USB-owner task -> application task. No router, link or history mutation here.
unsigned kms_usb_input_selected(void);
unsigned kms_usb_input_sources(void);
void kms_usb_input_block(bool clear_link);
void kms_usb_input_event(enum kms_event, unsigned slot, uint32_t detail);
void kms_usb_input_capture_event(kms_diag_record_t *, enum kms_event, unsigned slot, uint32_t detail);
void kms_usb_input_save(void);
