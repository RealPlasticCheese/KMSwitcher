#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Separate, bounded ESP-NOW envelope. HID wire protocol v2 stays unchanged.
enum { KMS_M_VERSION = 1, KMS_M_DATA = 192, KMS_M_HEADER = 30,
       KMS_M_MAX_WIRE = KMS_M_HEADER + KMS_M_DATA + 4,
       KMS_M_REPLY = 1, KMS_M_SUMMARY = 2, KMS_M_FORCE = 4 };
enum kms_management_op {
    KMS_M_IDENTITY = 1, KMS_M_STATS, KMS_M_HISTORY_BEGIN, KMS_M_HISTORY_READ,
    KMS_M_HISTORY_CLEAR, KMS_M_OTA_BEGIN, KMS_M_OTA_WRITE, KMS_M_OTA_END,
    KMS_M_OTA_ACTIVATE, KMS_M_OTA_ABORT, KMS_M_DESKTOP, KMS_M_CLIPBOARD,
    KMS_M_CACHE_INFO, KMS_M_CACHE_BEGIN, KMS_M_CACHE_WRITE, KMS_M_CACHE_END,
    KMS_M_CACHE_ABORT, KMS_M_CACHE_INSTALL, KMS_M_CACHE_PROGRESS, KMS_M_RESTART, KMS_M_RADIO_STATUS, KMS_M_STREAM_STATUS
};
enum { KMS_CAP_RESTART = 16, KMS_RESTART_KEY = 0x52535431u, KMS_RESTART_DELAY_MS = 1500u };
enum { KMS_CAP_RADIO_DIAGNOSTICS = 32, KMS_RADIO_STATUS_WORDS = 20, KMS_RESTART_AUTOMATIC = 0x4155544fu };
enum { KMS_CAP_RADIO_PROBE = 128 };
enum kms_management_result {
    KMS_M_OK, KMS_M_BAD_REQUEST, KMS_M_BUSY, KMS_M_WRONG_BOOT, KMS_M_OFFSET,
    KMS_M_INTEGRITY, KMS_M_STORAGE, KMS_M_UNSUPPORTED, KMS_M_CURRENT,
    KMS_M_EXPIRED, KMS_M_INCOMPATIBLE, KMS_M_TIMEOUT
};
typedef struct {
    uint32_t id, sender, target, offset;
    uint16_t length;
    uint8_t op, flags, status, slot;
    uint8_t data[KMS_M_DATA];
} kms_management_frame_t;
uint32_t kms_crc32(const void *, size_t);
uint32_t kms_crc32_extend(uint32_t previous, const void *, size_t);
void kms_put_u32(uint8_t *, uint32_t);
uint32_t kms_get_u32(const uint8_t *);
size_t kms_management_encode(const kms_management_frame_t *, uint8_t *, size_t);
bool kms_management_decode(kms_management_frame_t *, const uint8_t *, size_t);
bool kms_hex_decode(const char *, uint8_t *, size_t, size_t *);
void kms_hex_encode(const uint8_t *, size_t, char *);
const char *kms_management_result_name(unsigned);
typedef struct { uint32_t requested_at, sender, id; bool pending; } kms_restart_t;
unsigned kms_restart_request(kms_restart_t *, const kms_management_frame_t *, uint32_t boot, bool busy, uint32_t now);
bool kms_restart_take(kms_restart_t *, uint32_t now);

// Track direct receiver uploads separately from cache inspection/progress reads.
typedef struct { uint32_t boot, token, at; } kms_remote_upload_t;
bool kms_management_changes_firmware(unsigned op);
void kms_remote_upload_reply(kms_remote_upload_t *, const kms_management_frame_t *, unsigned result, uint32_t now);
bool kms_remote_upload_busy(const kms_remote_upload_t *, uint32_t peer_boot, uint32_t now);

// SHA-256 is over the exact downloadable .bin bytes, including its appended digest.
enum { KMS_IDENTITY_BYTES = 120, KMS_GIT_BYTES = 48, KMS_UTC_BYTES = 24 };
typedef struct {
    char git[KMS_GIT_BYTES], utc[KMS_UTC_BYTES];
    uint8_t sha256[32];
    uint32_t image_bytes, chip, capabilities;
} kms_firmware_identity_t;
void kms_identity_encode(const kms_firmware_identity_t *, uint8_t *);
bool kms_identity_decode(kms_firmware_identity_t *, const uint8_t *, size_t);

// Flash operations are explicit dependencies so interruptions/corruption can be
// exercised against a fake flash device with the production upload state machine.
typedef struct {
    bool (*begin)(void *, uint32_t);
    bool (*write)(void *, uint32_t, const uint8_t *, size_t);
    bool (*verify)(void *, uint32_t, const uint8_t *);
    bool (*activate)(void *);
    void (*abort)(void *);
    void *ctx;
} kms_upload_io_t;
typedef struct {
    kms_upload_io_t io;
    uint32_t token, size, offset, last_offset, last_size, last_crc;
    uint8_t sha256[32];
    uint8_t last_data[KMS_M_DATA - 4];
    bool active, verified, activated;
} kms_upload_t;
void kms_upload_init(kms_upload_t *, const kms_upload_io_t *);
unsigned kms_upload_begin(kms_upload_t *, uint32_t, uint32_t, const uint8_t *, bool,
                          const uint8_t *, uint32_t);
unsigned kms_upload_write(kms_upload_t *, uint32_t, uint32_t, const uint8_t *, size_t);
unsigned kms_upload_end(kms_upload_t *, uint32_t);
unsigned kms_upload_activate(kms_upload_t *, uint32_t);
void kms_upload_abort(kms_upload_t *);
enum { KMS_BOOT_WAIT, KMS_BOOT_CONFIRM, KMS_BOOT_ROLLBACK };
unsigned kms_boot_health_decide(uint32_t start, uint32_t now, bool app, bool usb, bool radio,
                                uint32_t grace, uint32_t deadline);
