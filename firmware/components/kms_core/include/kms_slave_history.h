#pragma once
#include "kms_core.h"

enum kms_slave_event {
    KMS_S_BOOT, KMS_S_USB_MOUNT, KMS_S_USB_SUSPEND, KMS_S_USB_RESUME,
    KMS_S_USB_UNMOUNT, KMS_S_USB_STALL, KMS_S_USB_OVERFLOW, KMS_S_RX_OVERFLOW,
    KMS_S_LEASE_EXPIRED, KMS_S_OUTPUT_CLEAR, KMS_S_RADIO_ERROR,
    KMS_S_UPDATE_BEGIN, KMS_S_UPDATE_VERIFIED, KMS_S_UPDATE_ABORT,
    KMS_S_UPDATE_CONFIRMED, KMS_S_STORAGE_ERROR, KMS_S_EVENT_COUNT
};
enum kms_slave_metric {
    KMS_S_RX, KMS_S_TX, KMS_S_DUPLICATES, KMS_S_REJECTED, KMS_S_ACCEPTED,
    KMS_S_COMPLETED, KMS_S_PEAK_RX, KMS_S_PEAK_USB, KMS_S_MAX_USB_MS,
    KMS_S_MIN_HEAP, KMS_S_USB_FLAGS, KMS_S_METRIC_COUNT
};
typedef struct {
    uint32_t sequence, boot, first_ms, last_ms, detail, repeats, event;
} kms_slave_event_record_t;
typedef struct {
    // The fourth word was reserved in journal v2; retain its layout for migration.
    uint32_t boot, reset, sequence, radio_failed;
    uint64_t uptime_ms;
    uint32_t metrics[KMS_S_METRIC_COUNT], totals[KMS_S_EVENT_COUNT];
    int32_t rssi_last, rssi_mean;
    uint32_t last_radio_ms;
} kms_slave_stats_t;
typedef struct {
    uint32_t version, event_capacity, stats_capacity, next_sequence;
    uint32_t next_event, event_count, events_overwritten;
    uint32_t next_stats, stats_count, stats_overwritten;
    kms_slave_event_record_t events[KMS_SLAVE_HISTORY_EVENTS];
    kms_slave_stats_t snapshots[KMS_SLAVE_HISTORY_STATS];
} kms_slave_journal_t;
typedef struct {
    kms_slave_journal_t journal;
    kms_slave_stats_t live;
    uint32_t last_tick, checkpoint;
    int64_t rssi_sum;
    uint32_t rssi_samples;
} kms_slave_history_t;
void kms_slave_history_init(kms_slave_history_t *, uint32_t, uint32_t, uint32_t);
bool kms_slave_history_valid(const kms_slave_journal_t *);
void kms_slave_history_tick(kms_slave_history_t *, uint32_t);
bool kms_slave_history_event(kms_slave_history_t *, uint32_t, unsigned, uint32_t);
void kms_slave_history_checkpoint(kms_slave_history_t *);
void kms_slave_history_clear(kms_slave_history_t *);
const char *kms_slave_event_name(unsigned);
enum { KMS_RF_MAC_FAILED = 1, KMS_RF_TX_SLOW, KMS_RF_RX_GAP, KMS_RF_DISPATCH,
       KMS_RF_MAIN_GAP, KMS_RF_RESTART, KMS_RF_AUTO_RESTART, KMS_RF_REASONS };
#define KMS_RADIO_FAULT_TAG 0xa0000000u
uint32_t kms_slave_radio_detail(unsigned reason, unsigned op, uint32_t duration);
