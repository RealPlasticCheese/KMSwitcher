#pragma once
#include "kms_desktop.h"
#include "kms_clipboard.h"

void kms_desktop_init(bool master, uint32_t boot);
void kms_desktop_link_tick(unsigned slot, uint32_t master_boot, uint32_t generation, bool active, bool usb_ready, bool idle);
void kms_desktop_receive(const kms_management_frame_t *);
bool kms_desktop_take_outgoing(kms_management_frame_t *);
typedef struct {
    uint32_t usb_received, radio_received, unauthorized, relayed, delivered, queue_drops;
    unsigned commands, incoming, outgoing;
} kms_clipboard_diagnostics_t;
void kms_clipboard_diagnostics(kms_clipboard_diagnostics_t *);
void kms_clipboard_receive(const kms_management_frame_t *);
void kms_clipboard_usb_set(const uint8_t *, size_t);
size_t kms_clipboard_usb_get(uint8_t *, size_t);
void kms_desktop_input(const kms_sample_t *);
void kms_desktop_motion(int32_t x, int32_t y);
void kms_desktop_block(bool blocked);
bool kms_desktop_jiggler_allowed(void);
// USB task callbacks only copy bounded data; all radio/NVS work stays on app task.
void kms_desktop_usb_reset(void);
void kms_desktop_usb_set(const uint8_t *, size_t);
void kms_desktop_local_reply(const kms_desktop_message_t *);
size_t kms_desktop_usb_get(unsigned report, uint8_t *, size_t);
typedef bool (*kms_desktop_preferences_fn)(bool write, uint8_t data[KMS_PREFERENCES_BYTES], uint32_t *revision);
void kms_desktop_preferences_handler(kms_desktop_preferences_fn);
// Controller application callback: false means a worker will reply later.
typedef bool (*kms_desktop_device_fn)(unsigned source, uint32_t boot, kms_desktop_message_t *);
void kms_desktop_device_handler(kms_desktop_device_fn);
// Call on the application task; stale host sessions are discarded.
void kms_desktop_device_reply(unsigned source, uint32_t boot, const kms_desktop_message_t *);
