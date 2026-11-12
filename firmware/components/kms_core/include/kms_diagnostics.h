#pragma once
#include "kms_core.h"

enum kms_event {
    KMS_EVENT_BOOT, KMS_EVENT_ACK_TIMEOUT, KMS_EVENT_PEER_LOST,
    KMS_EVENT_PEER_RESTART, KMS_EVENT_USB_UNAVAILABLE, KMS_EVENT_TX_OVERFLOW,
    KMS_EVENT_RX_OVERFLOW, KMS_EVENT_USB_STALL, KMS_EVENT_LEASE_EXPIRED,
    KMS_EVENT_INPUT_REMOVED, KMS_EVENT_INPUT_ERROR, KMS_EVENT_INPUT_OVERFLOW,
    KMS_EVENT_LED_FAILED, KMS_EVENT_LED_PENDING, KMS_EVENT_REPLAY_DROPPED,
    KMS_EVENT_RECOVERED, KMS_EVENT_SELECT_UNAVAILABLE, KMS_EVENT_SELECTED,
    KMS_EVENT_COUNT
};
// Tagged details extend USB-host history without changing persisted event/stat
// sizes or radio management layouts. Bytes: 55 / phase / USB address / interface.
#define KMS_USB_HOST_EVENT_TAG 0x55000000u
#define KMS_USB_FAULT_EVENT_TAG 0x56000000u
enum kms_usb_fault_field {
    KMS_USB_FAULT_OPERATION = 1, KMS_USB_FAULT_INTERFACE, KMS_USB_FAULT_PENDING,
    KMS_USB_FAULT_PORT_LOW, KMS_USB_FAULT_PORT_HIGH, KMS_USB_FAULT_FRAME_AGE,
    KMS_USB_FAULT_REQUEST, KMS_USB_FAULT_VALUE, KMS_USB_FAULT_INDEX,
    KMS_USB_FAULT_VID, KMS_USB_FAULT_PID
};
enum kms_usb_host_event {
    KMS_USB_HOST_OVERDUE = 1, KMS_USB_HOST_RESET, KMS_USB_HOST_RESTORED,
    KMS_USB_HOST_FAILED, KMS_USB_HOST_RESTART, KMS_USB_HOST_ABSENT, KMS_USB_HOST_CALL_TIMEOUT
};
typedef struct {
    uint32_t version, sequence, boot, uptime_ms, idle_ms, detail, repeats;
    uint16_t event, slot;
} kms_diag_record_t;
typedef struct {
    uint32_t version, sequence, boot, reset_reason;
    uint64_t uptime_ms, idle_ms, max_idle_ms, recovery_ms;
    uint32_t reports, activity, radio_tx, radio_rx, retries, replayed, dropped;
    uint32_t peak_buffer, storage_errors, events[KMS_EVENT_COUNT];
} kms_diag_stats_t;
typedef struct {
    kms_diag_stats_t stats;
    kms_diag_record_t records[KMS_DIAGNOSTICS_EVENTS];
    uint32_t last_tick, last_input, checkpoint, next_sequence;
    unsigned next;
    bool held;
} kms_diag_model_t;
const char *kms_event_name(unsigned);
const char *kms_diag_event_name(unsigned event, uint32_t detail);
void kms_diag_model_init(kms_diag_model_t *, uint32_t now, uint32_t boot, uint32_t reset);
void kms_diag_model_tick(kms_diag_model_t *, uint32_t now);
void kms_diag_model_input(kms_diag_model_t *, uint32_t now, bool changed, bool held);
unsigned kms_diag_model_event(kms_diag_model_t *, uint32_t now, enum kms_event, unsigned slot, uint32_t detail);
bool kms_diag_model_checkpoint(kms_diag_model_t *, uint32_t now);
