#pragma once
#include "kms_core.h"

enum { KMS_JIGGLER_DEFAULT_SECONDS = 30, KMS_JIGGLER_MAX_SECONDS = 65535, KMS_JIGGLER_MAX_DISTANCE = 127 };
enum kms_jiggler_mode { KMS_JIGGLER_LEFT_RIGHT, KMS_JIGGLER_FOUR_WAY, KMS_JIGGLER_RANDOM, KMS_JIGGLER_MODE_COUNT };
enum { KMS_JIGGLER_RANDOM_PERIOD_MS = 20 };
typedef struct { int32_t x, y; } kms_jiggler_motion_t;
const char *kms_jiggler_mode_name(unsigned mode);
// Normal selected-dongle indicator: 150 ms off (or blue) every five seconds.
unsigned kms_jiggler_indicator(uint32_t now, unsigned normal_color);

typedef struct {
    uint32_t session, sequence, changed, random;
    uint16_t interval_seconds;
    uint8_t distance, mode, phase, remaining;
    bool enabled, right, running;
} kms_jiggler_t;

// The caller authenticates the paired sender and checks the receiver boot ID.
bool kms_jiggler_configure(kms_jiggler_t *, const kms_packet_t *, uint32_t now);
// Returns the next relative motion vector when due. Commit after USB accepts it.
bool kms_jiggler_motion(const kms_jiggler_t *, uint32_t now, kms_jiggler_motion_t *);
void kms_jiggler_moved(kms_jiggler_t *, uint32_t now);
// Real mouse motion postpones the next jiggle without changing its direction.
void kms_jiggler_activity(kms_jiggler_t *, uint32_t now);

typedef struct {
    uint32_t boot, sequence, sent_at;
    uint16_t interval_seconds;
    uint8_t distance, mode;
    bool known, enabled, pending, sent, distance_supported, modes_supported;
} kms_jiggler_sync_t;

void kms_jiggler_sync_request(kms_jiggler_sync_t *, bool enabled, uint16_t seconds, uint8_t distance, unsigned mode);
void kms_jiggler_sync_status(kms_jiggler_sync_t *, const kms_packet_t *, uint32_t master, uint16_t seconds, uint8_t distance);
bool kms_jiggler_sync_packet(const kms_jiggler_sync_t *, uint32_t master, unsigned slot, uint32_t now, kms_packet_t *);
