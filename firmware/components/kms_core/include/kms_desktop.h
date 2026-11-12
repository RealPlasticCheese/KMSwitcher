#pragma once
#include "kms_core.h"
#include "kms_management.h"

enum { KMS_D_VERSION = 1, KMS_D_BYTES = 64, KMS_D_DATA = 48,
       KMS_D_COMMAND_REPORT = 32, KMS_D_STATUS_REPORT = 33,
       KMS_D_MONITORS = 16, KMS_D_CONFIG_BYTES = 96,
       KMS_D_SCREENS_BYTES = 12 + KMS_D_MONITORS * 16,
       KMS_D_LEGACY_LAYOUT_BYTES = KMS_D_CONFIG_BYTES + KMS_SLOTS * KMS_D_SCREENS_BYTES + 4,
       KMS_D_LAYOUT_BYTES = KMS_D_LEGACY_LAYOUT_BYTES + 16,
       KMS_D_DEFAULT_TOLERANCE = 8,
       KMS_D_LEASE_MS = 3000, KMS_D_HANDOFF_MS = 300 };
#define KMS_D_CONFIG_TOLERANCES 0x314c4f54u // TOL1
// Extended KMS_D_PEER requests use this flag and an opaque span cursor.
#define KMS_D_PEER_JOIN_PREVIEW 0x80000000u
#define KMS_D_PEER_ALL_ROUTES 0x40000000u
enum kms_desktop_op {
    KMS_D_HELLO = 1, KMS_D_HEARTBEAT, KMS_D_SCREENS_BEGIN, KMS_D_SCREEN,
    KMS_D_SCREENS_COMMIT, KMS_D_CONFIG_READ, KMS_D_CONFIG_BEGIN,
    KMS_D_CONFIG_CHUNK, KMS_D_CONFIG_COMMIT, KMS_D_PEER,
    KMS_D_CROSS, KMS_D_PREPARE, KMS_D_READY, KMS_D_SELECT,
    KMS_D_PREFERENCES_GET, KMS_D_PREFERENCES_SET, KMS_D_STATUS,
    KMS_D_LOCAL_HELLO, KMS_D_LOG_BEGIN, KMS_D_LOG_READ, KMS_D_LOG_END, KMS_D_RECOVERY_STATUS, KMS_D_POWER,
    KMS_D_CHANNEL_GET, KMS_D_CHANNEL_SET, KMS_D_CHANNEL_RESUME,
    KMS_D_DEVICE_CONTROL, KMS_D_USB_SNAPSHOT, KMS_D_USB_DEVICE,
    KMS_D_USB_INTERFACE, KMS_D_USB_CHANGE, KMS_D_REMOTE_REQUEST,
    KMS_D_REMOTE_READ, KMS_D_PREFERENCES_DEFAULT, KMS_D_CHANNEL_RECOVERY_GET, KMS_D_CHANNEL_RECOVERY_SET
};
// OS reports describe intent, not proof that a transition completed.
enum kms_desktop_power { KMS_D_POWER_UNKNOWN, KMS_D_POWER_AWAKE,
    KMS_D_POWER_SLEEP_OR_HIBERNATE, KMS_D_POWER_SHUTDOWN };
enum kms_desktop_result { KMS_D_OK, KMS_D_INVALID, KMS_D_STALE, KMS_D_BUSY,
    KMS_D_OFFLINE, KMS_D_STORAGE, KMS_D_UNSUPPORTED, KMS_D_CONFLICT };
enum kms_desktop_direction { KMS_D_LEFT, KMS_D_RIGHT, KMS_D_UP, KMS_D_DOWN };
typedef struct { uint8_t op, result; uint32_t id, session; uint8_t data[KMS_D_DATA]; } kms_desktop_message_t;
typedef struct { int32_t x, y, width, height; } kms_desktop_rect_t;
typedef struct { int32_t x, y; uint16_t width, height; bool configured; } kms_desktop_station_t;
typedef struct {
    bool enabled; uint16_t delay_ms; uint8_t corner;
    kms_desktop_station_t stations[KMS_SLOTS];
    uint32_t gap_tolerance, overlap_tolerance;
} kms_desktop_config_t;
typedef struct {
    uint32_t session, boot, seen, geometry, staging_geometry, mask;
    bool enabled, ready; uint8_t count, staging_count;
    uint8_t power; uint32_t power_seen;
    kms_desktop_rect_t screens[KMS_D_MONITORS], staging[KMS_D_MONITORS];
} kms_desktop_peer_t;
typedef struct {
    uint8_t count;
    kms_desktop_rect_t screens[KMS_D_MONITORS];
} kms_desktop_reference_t;
typedef struct kms_desktop_master kms_desktop_master_t;
typedef struct {
    bool (*send)(void *, unsigned, const kms_desktop_message_t *);
    bool (*save)(void *, const kms_desktop_config_t *, const kms_desktop_master_t *, uint8_t reference_mask);
    bool (*save_screens)(void *, unsigned, const uint8_t *);
    bool (*select)(void *, unsigned);
    // Automatic destinations only; manual selection retains its existing policy.
    bool (*available)(void *, unsigned);
    void *ctx;
    struct kms_desktop_routes *routes;
} kms_desktop_io_t;
struct kms_desktop_master {
    kms_desktop_io_t io; kms_desktop_config_t config;
    kms_desktop_peer_t peers[KMS_SLOTS];
    kms_desktop_reference_t reference[KMS_SLOTS];
    uint32_t reference_gap, reference_overlap;
    uint32_t revision, selected, generation, topology_revision; uint8_t availability; bool idle;
    struct { bool active; uint32_t id, request_id, source, target, session, destination_session, generation, topology, at, sent;
        int32_t x, y, landing_x, landing_y; uint8_t direction, excluded; } handoff;
    // The source receives its CROSS request ID only while this exact edge-selected route remains current.
    struct { uint32_t source, session, request_id, target, generation; } edge_departure;
    struct { uint32_t slot, session, id, revision, at, gap_tolerance, overlap_tolerance;
        bool refresh_joins; uint8_t mask, data[KMS_D_CONFIG_BYTES]; } upload;
    uint32_t sequence, last_source, last_id, last_session; uint8_t last_result;
};

bool kms_desktop_encode(const kms_desktop_message_t *, uint8_t *);
bool kms_desktop_decode(kms_desktop_message_t *, const uint8_t *, size_t);
bool kms_desktop_config_encode(const kms_desktop_config_t *, uint8_t *);
bool kms_desktop_config_decode(kms_desktop_config_t *, const uint8_t *, size_t);
bool kms_desktop_screens_encode(const kms_desktop_rect_t *, unsigned, uint8_t *);
bool kms_desktop_screens_decode(kms_desktop_peer_t *, const uint8_t *, size_t);
void kms_desktop_capture_reference(kms_desktop_master_t *, uint8_t mask);
bool kms_desktop_layout_encode(const kms_desktop_master_t *, const kms_desktop_config_t *, uint8_t, uint8_t *);
bool kms_desktop_layout_decode(kms_desktop_master_t *, const uint8_t *, size_t);
void kms_desktop_master_init(kms_desktop_master_t *, const kms_desktop_io_t *, uint32_t);
void kms_desktop_master_request(kms_desktop_master_t *, unsigned, uint32_t, const kms_desktop_message_t *, uint32_t);
void kms_desktop_master_tick(kms_desktop_master_t *, unsigned, uint32_t, bool, uint32_t);
void kms_desktop_master_motion(kms_desktop_master_t *, int32_t, int32_t);
