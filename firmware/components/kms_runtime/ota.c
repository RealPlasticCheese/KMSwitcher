#include "kms_ota.h"
#include "kms_build.h"
#include "kms_runtime.h"
#include "kms_slave_diagnostics.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static bool controller;
static void record_event(unsigned event, uint32_t detail)
{
    if (controller) ESP_LOGI("ota", "Master update event=%u detail=%lu", event, (unsigned long)detail);
    else kms_slave_diagnostics_event(event, detail);
}
static kms_upload_t upload;
static const esp_partition_t *destination;
static esp_ota_handle_t handle;
static uint32_t last_activity, reboot_at, health_started, owner;
static bool pending_confirmation, reboot_pending;
static bool updating;
static portMUX_TYPE update_lock = portMUX_INITIALIZER_UNLOCKED;
static void set_updating(bool value)
{ portENTER_CRITICAL(&update_lock); updating = value; portEXIT_CRITICAL(&update_lock); }
bool kms_ota_busy(void)
{
    portENTER_CRITICAL(&update_lock); bool value = updating; portEXIT_CRITICAL(&update_lock);
    return value;
}

static bool begin(void *ctx, uint32_t size)
{
    (void)ctx; destination = esp_ota_get_next_update_partition(NULL);
    set_updating(true);
    return destination && esp_ota_begin(destination, size, &handle) == ESP_OK;
}
static bool write_chunk(void *ctx, uint32_t offset, const uint8_t *data, size_t bytes)
{ (void)ctx; (void)offset; return handle && esp_ota_write(handle, data, bytes) == ESP_OK; }
static bool verify(void *ctx, uint32_t size, const uint8_t *expected)
{
    (void)ctx;
    uint8_t actual[32];
    if (!kms_build_hash_partition(destination, size, actual) || memcmp(actual, expected, 32)) return false;
    esp_partition_pos_t position = {.offset = destination->address, .size = destination->size};
    esp_image_metadata_t metadata;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK ||
        metadata.image_len != size || metadata.image.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return false;
    esp_app_desc_t app;
    if (esp_ota_get_partition_description(destination, &app) != ESP_OK ||
        strcmp(app.project_name, controller ? "kmswitcher_controller" : "kmswitcher_receiver")) return false;
    esp_err_t result = esp_ota_end(handle); handle = 0;
    return result == ESP_OK;
}
static bool activate(void *ctx)
{ (void)ctx; return destination && esp_ota_set_boot_partition(destination) == ESP_OK; }
static void abort_upload(void *ctx)
{ (void)ctx; if (handle) { esp_ota_abort(handle); handle = 0; } set_updating(false); }
void kms_ota_init(bool master)
{
    controller = master;
    const kms_upload_io_t io = {begin, write_chunk, verify, activate, abort_upload, NULL};
    kms_upload_init(&upload, &io);
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    pending_confirmation = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                           state == ESP_OTA_IMG_PENDING_VERIFY;
    health_started = kms_now();
}
bool kms_ota_confirmed(void) { return !pending_confirmation; }
void kms_ota_startup_failed(void)
{
    if (!pending_confirmation) return;
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();
}
void kms_ota_request(const kms_management_frame_t *q, kms_management_frame_t *r)
{
    r->status = KMS_M_BAD_REQUEST;
    if (pending_confirmation) { r->status = KMS_M_BUSY; return; }
    uint32_t token = q->length >= 4 ? kms_get_u32(q->data) : 0;
    bool active_at_entry = upload.active;
    if (upload.active && q->sender != owner) { r->status = KMS_M_BUSY; return; }
    if (q->op == KMS_M_OTA_BEGIN && q->length == 44) {
        if (!(kms_build_identity()->capabilities & 2)) { r->status = KMS_M_UNSUPPORTED; return; }
        if (kms_get_u32(q->data + 40) != CONFIG_IDF_FIRMWARE_CHIP_ID) {
            r->status = KMS_M_INCOMPATIBLE; return;
        }
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (!next) { r->status = KMS_M_UNSUPPORTED; return; }
        bool was_active = upload.active;
        r->status = (uint8_t)kms_upload_begin(&upload, token, kms_get_u32(q->data + 4), q->data + 8,
            (q->flags & KMS_M_FORCE) != 0, kms_build_identity()->sha256, next->size);
        if (r->status == KMS_M_OK) {
            owner = q->sender;
            if (!was_active) record_event(KMS_S_UPDATE_BEGIN, upload.size);
        }
    } else if (q->op == KMS_M_OTA_WRITE && q->length > 4) {
        r->status = (uint8_t)kms_upload_write(&upload, token, q->offset, q->data + 4, q->length - 4);
    } else if (q->op == KMS_M_OTA_END && q->length == 4) {
        bool was_verified = upload.verified;
        r->status = (uint8_t)kms_upload_end(&upload, token);
        if (r->status == KMS_M_OK && !was_verified) record_event(KMS_S_UPDATE_VERIFIED, upload.size);
    } else if (q->op == KMS_M_OTA_ACTIVATE && q->length == 4) {
        r->status = (uint8_t)kms_upload_activate(&upload, token);
        if (r->status == KMS_M_OK && !reboot_pending) { reboot_pending = true; reboot_at = kms_now(); }
    } else if (q->op == KMS_M_OTA_ABORT && q->length == 4) {
        if (upload.activated) r->status = KMS_M_BUSY;
        else if (!upload.active || token != upload.token) r->status = KMS_M_EXPIRED;
        else {
            kms_upload_abort(&upload); r->status = KMS_M_OK;
            record_event(KMS_S_UPDATE_ABORT, 0);
        }
    }
    if (r->status == KMS_M_OK) last_activity = kms_now();
    if ((active_at_entry && !upload.active && q->op != KMS_M_OTA_ABORT) ||
        (q->op == KMS_M_OTA_BEGIN && r->status == KMS_M_STORAGE))
        record_event(KMS_S_UPDATE_ABORT, r->status);
    set_updating(upload.active);
    r->length = 4; kms_put_u32(r->data, upload.offset);
}
void kms_ota_tick(bool app_alive, bool usb_initialized, bool radio_seen)
{
    uint32_t now = kms_now();
    if (reboot_pending && kms_time_elapsed(now, reboot_at, 1500)) esp_restart();
    if (pending_confirmation) {
        unsigned action = kms_boot_health_decide(health_started, now, app_alive, usb_initialized, radio_seen,
                                                KMS_OTA_HEALTH_MS, KMS_OTA_HEALTH_DEADLINE_MS);
        if (action == KMS_BOOT_CONFIRM) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                pending_confirmation = false;
                record_event(KMS_S_UPDATE_CONFIRMED, 0);
            }
        }
        if (action == KMS_BOOT_ROLLBACK) {
            kms_ota_startup_failed();
        }
    }
    if (upload.active && !upload.activated && kms_time_elapsed(now, last_activity, KMS_MANAGEMENT_SESSION_MS)) {
        kms_upload_abort(&upload); set_updating(false); record_event(KMS_S_UPDATE_ABORT, KMS_M_TIMEOUT);
    }
}
