#include "kms_remote.h"
#include "kms_runtime.h"
#include "kms_radio_channel.h"
#include "kms_build.h"
#include "kms_slave_diagnostics.h"
#include "kms_desktop_runtime.h"
#include "kms_jiggler_store.h"
#include "kms_ota.h"
#include "kms_receiver_cache.h"
#include "kms_cached_update.h"
#include "kms_memory.h"
#include "kms_stream_radio.h"
#include "kms_test_mode.h"
#include "nvs.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

enum job_type { LIST, STATUS, HISTORY, CLEAR, RAW, LOCAL, AUTO_RESTART, AUTO_CONFIG, DESKTOP_REMOTE };
_Static_assert(KMS_COMMAND_BYTES >= KMS_M_MAX_WIRE * 2 + 16, "Console buffer must hold a complete management frame");
typedef struct {
    unsigned type, slot;
    kms_management_frame_t frame;
    unsigned desktop_source;
    uint32_t desktop_boot, desktop_session, desktop_id;
} job_t;
typedef struct { kms_management_frame_t frame; uint32_t until; bool automatic, active; } outbound_t;
typedef struct { bool paired; uint8_t flags, mac[6]; uint32_t boot, seen, identity_seen;
    kms_firmware_identity_t identity; uint8_t summary[168]; uint32_t summary_seen; } peer_view_t;
static QueueHandle_t jobs, incoming, outgoing;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    uint32_t boot, session, id, at;
    bool busy;
    kms_management_frame_t reply;
} desktop_result_t;
static desktop_result_t *desktop_results;
static peer_view_t peers[KMS_SLOTS];
#if CONFIG_IDF_TARGET_ESP32S3
static const bool master = true;
#else
static const bool master = false;
#endif
static bool usb_initialized, radio_seen, input_active;
static kms_restart_t restart;
static bool restart_saved, restart_issued;
bool kms_remote_restart_pending(void)
{
    portENTER_CRITICAL(&state_lock); bool pending = restart.pending; portEXIT_CRITICAL(&state_lock);
    return !master && pending;
}
bool kms_remote_take_restart(void)
{
    portENTER_CRITICAL(&state_lock);
    bool due = !master && !restart_issued && restart_saved && kms_restart_take(&restart, kms_now());
    if (due) { restart_issued = true; restart.pending = true; }
    portEXIT_CRITICAL(&state_lock); return due;
}
static uint32_t boot_id, own_slot, app_tick, sequence, last_summary;
static const char *TAG = "remote";
static kms_slave_snapshot_t *snapshot;
static uint32_t snapshot_token, snapshot_owner, snapshot_size, snapshot_at;
typedef struct { kms_management_frame_t request, reply; uint32_t at; bool valid; } cached_reply_t;
enum { REPLY_CACHE_SIZE = KMS_STREAM_WINDOW };
static cached_reply_t *cache;
static unsigned cache_next;
static unsigned local_begins_queued;
static unsigned transfer_jobs_queued;
static unsigned manual_restart_jobs;
static uint32_t manual_restart_at;
static bool manual_restart_wait;
static kms_remote_upload_t remote_uploads[KMS_SLOTS];
static bool transfer_was_busy;
static kms_cached_update_t cached_update;
static bool cache_transfer_busy;
static kms_recovery_budget_t recovery_budgets[KMS_SLOTS];
static bool recovery_enabled = true, recovery_reserved, recovery_result_pending;
static kms_recovery_result_t recovery_result;
static uint32_t history_activity;
static bool history_active;
static uint32_t automatic_id, automatic_until;
static bool automatic_sending, automatic_active, automatic_dispatched;
static void cache_request(const kms_management_frame_t *, kms_management_frame_t *);
bool kms_remote_transfer_busy(void)
{
    if (!master) return false;
    uint32_t now = kms_now();
    portENTER_CRITICAL(&state_lock);
    bool busy = cache_transfer_busy || transfer_jobs_queued;
    for (unsigned i = 0; i < KMS_SLOTS; ++i)
        busy |= kms_remote_upload_busy(&remote_uploads[i], peers[i].boot, now);
    portEXIT_CRITICAL(&state_lock);
    return kms_remote_master_updating() || busy;
}
static bool changes_firmware(const job_t *job)
{ return (job->type == RAW || job->type == LOCAL) && kms_management_changes_firmware(job->frame.op); }
static bool local_seen;
static bool same_request(const kms_management_frame_t *, const kms_management_frame_t *);
bool kms_remote_master_updating(void)
{
    portENTER_CRITICAL(&state_lock); bool queued = local_begins_queued != 0; portEXIT_CRITICAL(&state_lock);
    return master && (queued || kms_ota_busy() || !kms_ota_confirmed());
}
bool kms_remote_master_restart_pending(void)
{
    uint32_t now = kms_now();
    portENTER_CRITICAL(&state_lock);
    if (manual_restart_wait && now - manual_restart_at >= KMS_RECOVERY_BOOT_WAIT_MS) manual_restart_wait = false;
    bool pending = manual_restart_jobs || manual_restart_wait;
    portEXIT_CRITICAL(&state_lock);
    return pending;
}
bool kms_remote_radio_probe_supported(unsigned slot, uint32_t boot)
{
    portENTER_CRITICAL(&state_lock);
    const peer_view_t *p = &peers[slot - 1];
    const unsigned required = KMS_CAP_RADIO_PROBE | 4u;
    bool supported = p->boot == boot && p->identity_seen && (p->identity.capabilities & required) == required;
    portEXIT_CRITICAL(&state_lock);
    return supported;
}
static void print_local(const kms_management_frame_t *reply)
{
    uint8_t wire[KMS_M_MAX_WIRE]; char hex[KMS_M_MAX_WIRE * 2 + 1];
    size_t bytes = kms_management_encode(reply, wire, sizeof(wire));
    kms_hex_encode(wire, bytes, hex); printf("KMSMASTER %s\n", hex);
}
static void local_request(const kms_management_frame_t *q)
{
    for (unsigned i = 0; cache && i < REPLY_CACHE_SIZE; ++i)
        if (cache[i].valid && !kms_time_elapsed(kms_now(), cache[i].at, KMS_MANAGEMENT_SESSION_MS) &&
            same_request(&cache[i].request, q)) { print_local(&cache[i].reply); return; }
    kms_management_frame_t r = {.op = q->op, .slot = q->slot, .id = q->id, .offset = q->offset,
        .sender = boot_id, .target = q->sender, .flags = KMS_M_REPLY};
    if (q->target != boot_id && !(q->target == 0 && q->op == KMS_M_IDENTITY)) r.status = KMS_M_WRONG_BOOT;
    else if (q->op == KMS_M_IDENTITY && !q->length) {
        kms_firmware_identity_t identity = *kms_build_identity();
        if (kms_ota_confirmed()) identity.capabilities |= 4;
        if (kms_receiver_cache_available()) identity.capabilities |= 8;
        kms_identity_encode(&identity, r.data); r.length = KMS_IDENTITY_BYTES; local_seen = true;
    } else if (q->op == KMS_M_STREAM_STATUS && q->length <= 1) {
        if (q->length && !kms_stream_set_profile(q->data[0])) r.status = KMS_M_BAD_REQUEST;
        else r.length = (uint16_t)kms_stream_radio_status(r.data, sizeof(r.data));
    } else if (q->op == KMS_M_RADIO_STATUS && !q->length) {
        r.length = (uint16_t)kms_link_radio_status(r.data, sizeof(r.data));
    } else if (q->op >= KMS_M_CACHE_INFO && q->op <= KMS_M_CACHE_PROGRESS) {
        if (!kms_ota_confirmed() || kms_ota_busy()) r.status = KMS_M_BUSY;
        else cache_request(q, &r);
    } else if (q->op >= KMS_M_OTA_BEGIN && q->op <= KMS_M_OTA_ABORT) {
        portENTER_CRITICAL(&state_lock); unsigned selected = own_slot; portEXIT_CRITICAL(&state_lock);
        if (q->op == KMS_M_OTA_BEGIN && (selected || kms_receiver_cache_busy() || kms_cached_update_busy(&cached_update))) r.status = KMS_M_BUSY;
        else kms_ota_request(q, &r);
    } else r.status = KMS_M_UNSUPPORTED;
    // BUSY can become ready without a new boot/request ID; do not cache it.
    if (r.status != KMS_M_BUSY && q->op != KMS_M_CACHE_INFO && q->op != KMS_M_CACHE_PROGRESS) {
        cached_reply_t *saved = &cache[cache_next++ % REPLY_CACHE_SIZE];
        saved->request = *q; saved->reply = r; saved->at = kms_now(); saved->valid = true;
    }
    portENTER_CRITICAL(&state_lock);
    cache_transfer_busy = kms_receiver_cache_busy() || kms_cached_update_busy(&cached_update);
    portEXIT_CRITICAL(&state_lock);
    print_local(&r);
}

static bool send_frame(const kms_management_frame_t *f)
{
    if (!(master && automatic_sending && f->op == KMS_M_RESTART && f->id == automatic_id))
        return kms_stream_enqueue(f);
    outbound_t item = {.frame = *f};
    if (master && automatic_sending && f->op == KMS_M_RESTART && f->id == automatic_id) {
        item.automatic = true; item.until = automatic_until; item.active = automatic_active;
    }
    return outgoing && xQueueSend(outgoing, &item, 0) == pdTRUE;
}
static uint32_t next_id(void) { if (!++sequence) ++sequence; return sequence; }
static peer_view_t peer_view(unsigned slot)
{
    peer_view_t result = {0};
    if (slot && slot <= KMS_SLOTS) {
        portENTER_CRITICAL(&state_lock); result = peers[slot - 1]; portEXIT_CRITICAL(&state_lock);
    }
    return result;
}
void kms_remote_peer(unsigned slot, bool paired, uint32_t boot, uint32_t seen, uint8_t flags, const uint8_t *mac)
{
    if (!slot || slot > KMS_SLOTS) return;
    portENTER_CRITICAL(&state_lock);
    peer_view_t *p = &peers[slot - 1];
    if (p->boot != boot || !paired) {
        memset(&p->identity, 0, sizeof(p->identity)); p->identity_seen = p->summary_seen = 0;
    }
    p->paired = paired; p->boot = boot; p->seen = seen; p->flags = flags;
    if (mac) memcpy(p->mac, mac, sizeof(p->mac));
    portEXIT_CRITICAL(&state_lock);
}
void kms_remote_receive(const kms_management_frame_t *f)
{
    if (!master) {
        portENTER_CRITICAL(&state_lock); radio_seen = true; portEXIT_CRITICAL(&state_lock);
    }
    if (incoming) (void)xQueueSend(incoming, f, 0); // Management congestion never faults HID.
}
bool kms_remote_take_outgoing(kms_management_frame_t *f)
{
    if (!master) return false;
    outbound_t item;
    for (unsigned i = 0; outgoing && i < KMS_MANAGEMENT_QUEUE_DEPTH && xQueueReceive(outgoing, &item, 0) == pdTRUE; ++i) {
        if (item.automatic) {
            peer_view_t peer = peer_view(item.frame.slot);
            portENTER_CRITICAL(&state_lock);
            bool allowed = automatic_sending && item.frame.id == automatic_id && recovery_enabled &&
                (item.active || kms_link_selected() != item.frame.slot);
            portEXIT_CRITICAL(&state_lock);
            if (!allowed || !peer.paired || peer.boot != item.frame.target || !(peer.flags & KMS_FLAG_USB_READY) ||
                kms_time_elapsed(kms_now(), peer.seen, KMS_PEER_TIMEOUT_MS) ||
                (int32_t)(kms_now() - item.until) >= 0 || kms_remote_transfer_busy() ||
                kms_link_pairing() || kms_radio_channel_recovering() || kms_test_mode_active()) continue;
            portENTER_CRITICAL(&state_lock); automatic_dispatched = true; portEXIT_CRITICAL(&state_lock);
        }
        *f = item.frame; return true;
    }
    return false;
}
typedef struct { uint32_t id, target, offset, token, at; uint8_t slot; } stream_request_t;
static stream_request_t stream_requests[KMS_STREAM_WINDOW];
static void stream_reset(unsigned slot)
{
    for (unsigned i = 0; i < KMS_STREAM_WINDOW; ++i)
        if (stream_requests[i].slot == slot) stream_requests[i].id = 0;
}
static void print_remote(const kms_management_frame_t *r)
{
    uint8_t wire[KMS_M_MAX_WIRE]; char hex[KMS_M_MAX_WIRE * 2 + 1];
    size_t bytes = kms_management_encode(r, wire, sizeof(wire));
    kms_hex_encode(wire, bytes, hex); printf("KMSM %u %s\n", r->slot, hex);
}
static bool stream_reply(const kms_management_frame_t *f)
{
    for (unsigned i = 0; i < KMS_STREAM_WINDOW; ++i) {
        stream_request_t *p = &stream_requests[i];
        if (!p->id || f->op != KMS_M_OTA_WRITE || f->id != p->id || f->slot != p->slot ||
            f->offset != p->offset || !(f->flags & KMS_M_REPLY) || f->target != boot_id ||
            (f->sender != p->target && f->status != KMS_M_WRONG_BOOT)) continue;
        kms_management_frame_t q = {.op = KMS_M_OTA_WRITE, .slot = p->slot, .target = p->target, .length = 4};
        kms_put_u32(q.data, p->token);
        portENTER_CRITICAL(&state_lock);
        kms_remote_upload_reply(&remote_uploads[p->slot - 1], &q, f->status, kms_now());
        portEXIT_CRITICAL(&state_lock);
        print_remote(f); p->id = 0; return true;
    }
    return false;
}
static void stream_forward(kms_management_frame_t *q)
{
    q->sender = boot_id;
    stream_request_t *available = NULL;
    for (unsigned i = 0; i < KMS_STREAM_WINDOW; ++i) {
        stream_request_t *p = &stream_requests[i];
        if (p->id == q->id && p->slot == q->slot) { available = p; break; }
        if (!p->id || kms_time_elapsed(kms_now(), p->at, KMS_MANAGEMENT_SESSION_MS)) available = p;
    }
    if (available && send_frame(q)) {
        *available = (stream_request_t){q->id, q->target, q->offset, kms_get_u32(q->data), kms_now(), q->slot};
    } else {
        kms_management_frame_t r = *q; r.sender = q->target; r.target = boot_id;
        r.flags = KMS_M_REPLY; r.status = KMS_M_BUSY; r.length = 0; print_remote(&r);
    }
}
static void accept_summary(const kms_management_frame_t *f)
{
    if (stream_reply(f)) return;
    peer_view_t current = peer_view(f->slot);
    if (current.boot != f->sender) return;
    if (f->op == KMS_M_STATS && f->status == KMS_M_OK && f->length == 168) {
        portENTER_CRITICAL(&state_lock);
        peer_view_t *p = &peers[f->slot - 1];
        if (p->boot == f->sender) { memcpy(p->summary, f->data, 168); p->summary_seen = kms_now(); }
        portEXIT_CRITICAL(&state_lock);
    }
    if (f->op == KMS_M_IDENTITY && f->status == KMS_M_OK) {
        kms_firmware_identity_t identity;
        if (kms_identity_decode(&identity, f->data, f->length)) {
            portENTER_CRITICAL(&state_lock);
            peer_view_t *p = &peers[f->slot - 1];
            if (p->boot == f->sender) { p->identity = identity; p->identity_seen = kms_now(); }
            portEXIT_CRITICAL(&state_lock);
        }
    }
    if (f->flags & KMS_M_SUMMARY) {
        // Return a paired heartbeat so inactive receivers can confirm first-boot
        // radio health too. It has no input-routing or diagnostic side effects.
        if (f->op == KMS_M_IDENTITY) {
            kms_management_frame_t ack = *f; ack.sender = boot_id; ack.target = f->sender;
            ack.flags = KMS_M_REPLY; ack.length = 0; send_frame(&ack);
        }
    }
}
static unsigned rpc_attempts(kms_management_frame_t *request, kms_management_frame_t *reply, unsigned attempts)
{
    peer_view_t p = peer_view(request->slot);
    if (!p.paired) return KMS_M_UNSUPPORTED;
    request->sender = boot_id;
    if (!request->target) request->target = p.boot;
    if (request->op == KMS_M_RESTART && (kms_remote_master_updating() || kms_receiver_cache_busy() ||
        kms_cached_update_busy(&cached_update) || kms_link_pairing() || kms_radio_channel_recovering())) {
        *reply = *request; reply->sender = request->target; reply->target = boot_id;
        reply->flags = KMS_M_REPLY; reply->status = KMS_M_BUSY; reply->length = 0;
        return KMS_M_BUSY;
    }
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        if (!send_frame(request)) vTaskDelay(pdMS_TO_TICKS(KMS_MANAGEMENT_PACE_MS));
        uint32_t start = kms_now();
        while (!kms_time_elapsed(kms_now(), start, KMS_MANAGEMENT_RETRY_MS)) {
            kms_management_frame_t f;
            if (xQueueReceive(incoming, &f, pdMS_TO_TICKS(20)) != pdTRUE) continue;
            accept_summary(&f);
            if ((f.flags & KMS_M_REPLY) && !(f.flags & KMS_M_SUMMARY) && f.target == boot_id &&
                f.slot == request->slot && f.id == request->id && f.op == request->op &&
                (!request->target || f.sender == request->target || f.status == KMS_M_WRONG_BOOT)) {
                *reply = f; return f.status;
            }
        }
    }
    return KMS_M_TIMEOUT;
}
static unsigned rpc(kms_management_frame_t *q, kms_management_frame_t *r)
{ return rpc_attempts(q, r, KMS_MANAGEMENT_ATTEMPTS); }
static bool recovery_save(bool enabled, const kms_recovery_budget_t *budgets)
{
    uint8_t data[KMS_RECOVERY_STORE_BYTES];
    if (!kms_recovery_store_encode(enabled, budgets, data)) return false;
    nvs_handle_t h;
    if (nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t error = nvs_set_blob(h, "rx_recovery", data, sizeof(data));
    if (error == ESP_OK) error = nvs_commit(h);
    nvs_close(h); return error == ESP_OK;
}
static void recovery_load(void)
{
    uint8_t data[KMS_RECOVERY_STORE_BYTES]; size_t size = sizeof(data); nvs_handle_t h;
    if (nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        recovery_enabled = false; ESP_LOGW(TAG, "KMSRECOVERY disabled: budget storage unavailable"); return;
    }
    esp_err_t error = nvs_get_blob(h, "rx_recovery", data, &size); nvs_close(h);
    if (error == ESP_ERR_NVS_NOT_FOUND) return;
    if (error != ESP_OK || !kms_recovery_store_decode(&recovery_enabled, recovery_budgets, data, size)) {
        recovery_enabled = false; ESP_LOGW(TAG, "KMSRECOVERY disabled: invalid persisted budget");
    }
}
bool kms_remote_recovery_enabled(void)
{
    portENTER_CRITICAL(&state_lock); bool enabled = recovery_enabled; portEXIT_CRITICAL(&state_lock); return enabled;
}
void kms_remote_recovery_budget(unsigned slot, const uint8_t mac[6], kms_recovery_budget_t *out)
{
    memset(out, 0, sizeof(*out)); memcpy(out->mac, mac, 6);
    if (!slot || slot > KMS_SLOTS) return;
    portENTER_CRITICAL(&state_lock);
    if (!memcmp(recovery_budgets[slot - 1].mac, mac, 6)) *out = recovery_budgets[slot - 1];
    portEXIT_CRITICAL(&state_lock);
}
bool kms_remote_recovery_supported(unsigned slot, uint32_t boot, bool *confirmed)
{
    peer_view_t p = peer_view(slot);
    bool current = p.paired && p.boot == boot && p.identity_seen &&
        !kms_time_elapsed(kms_now(), p.identity_seen, 15000u);
    *confirmed = current && (p.identity.capabilities & 4);
    return current && (p.identity.capabilities & KMS_CAP_RESTART);
}
bool kms_remote_recovery_request(unsigned slot, uint32_t boot, const kms_recovery_budget_t *budget, bool active)
{
    if (!jobs || !slot || slot > KMS_SLOTS || !kms_remote_recovery_enabled()) return false;
    portENTER_CRITICAL(&state_lock);
    bool reserved = recovery_reserved;
    if (!reserved) recovery_reserved = true;
    portEXIT_CRITICAL(&state_lock);
    if (reserved) return false;
    job_t job = {.type = AUTO_RESTART, .slot = slot};
    job.frame.target = boot; job.frame.flags = active; memcpy(job.frame.data, budget, sizeof(*budget));
    if (xQueueSend(jobs, &job, 0) == pdTRUE) return true;
    portENTER_CRITICAL(&state_lock); recovery_reserved = false; portEXIT_CRITICAL(&state_lock); return false;
}
bool kms_remote_take_recovery_result(kms_recovery_result_t *out)
{
    portENTER_CRITICAL(&state_lock);
    bool pending = recovery_result_pending;
    if (pending) { *out = recovery_result; recovery_result_pending = recovery_reserved = false; }
    portEXIT_CRITICAL(&state_lock); return pending;
}
static void observe_upload(const kms_management_frame_t *q, unsigned result)
{
    portENTER_CRITICAL(&state_lock);
    kms_remote_upload_reply(&remote_uploads[q->slot - 1], q, result, kms_now());
    portEXIT_CRITICAL(&state_lock);
}
static unsigned cached_exchange(void *ctx, kms_management_frame_t *q, kms_management_frame_t *r)
{
    (void)ctx; unsigned result = rpc(q, r);
    observe_upload(q, result); return result;
}
static unsigned cached_exchange_window(void *ctx, kms_management_frame_t *frames, unsigned count)
{
    (void)ctx;
    bool acknowledged[KMS_STREAM_WINDOW] = {0};
    uint32_t progress_at = kms_now(), retry_at = 0, delay = 800;
    unsigned remaining = count;
    for (unsigned i = 0; i < count; ++i) frames[i].sender = boot_id;
    while (remaining && !kms_time_elapsed(kms_now(), progress_at, 30000)) {
        uint32_t now = kms_now();
        if (!retry_at || (int32_t)(now - retry_at) >= 0) {
            for (unsigned i = 0; i < count; ++i) if (!acknowledged[i]) (void)send_frame(&frames[i]);
            retry_at = now + delay; delay = delay < 1600 ? delay * 2 : 2000;
        }
        kms_management_frame_t r;
        if (xQueueReceive(incoming, &r, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        accept_summary(&r);
        for (unsigned i = 0; i < count; ++i) {
            kms_management_frame_t *q = &frames[i];
            if (acknowledged[i] || r.slot != q->slot || r.id != q->id || r.op != q->op ||
                r.offset != q->offset || r.target != boot_id || !(r.flags & KMS_M_REPLY) ||
                (r.sender != q->target && r.status != KMS_M_WRONG_BOOT)) continue;
            if (r.status == KMS_M_BUSY || r.status == KMS_M_OFFSET) continue;
            if (r.status != KMS_M_OK) return r.status;
            if (r.length != 4 || kms_get_u32(r.data) != q->offset + q->length - 4u) return KMS_M_OFFSET;
            acknowledged[i] = true; --remaining; observe_upload(q, r.status);
            progress_at = kms_now(); delay = 800;
        }
    }
    return remaining ? KMS_M_TIMEOUT : KMS_M_OK;
}
static bool cached_read(void *ctx, uint32_t offset, void *data, size_t bytes)
{ (void)ctx; return kms_receiver_cache_read(offset, data, bytes); }
static void cache_request(const kms_management_frame_t *q, kms_management_frame_t *r)
{
    r->status = KMS_M_BAD_REQUEST;
    if (!kms_receiver_cache_available()) { r->status = KMS_M_UNSUPPORTED; return; }
    if (q->op == KMS_M_CACHE_INSTALL && q->length == 36) {
        unsigned slot = kms_get_u32(q->data);
        if (!slot || slot > KMS_SLOTS || !peer_view(slot).paired) { r->status = KMS_M_UNSUPPORTED; return; }
        if (kms_cached_update_busy(&cached_update) || kms_receiver_cache_busy()) { r->status = KMS_M_BUSY; return; }
        const kms_firmware_identity_t *image = kms_receiver_cache_identity();
        if (!image) { r->status = KMS_M_EXPIRED; return; }
        if (memcmp(q->data + 4, image->sha256, 32)) { r->status = KMS_M_INTEGRITY; return; }
        if (kms_link_selected() == slot || kms_link_pairing()) { r->status = KMS_M_BUSY; return; }
        const kms_cached_update_io_t io = {.exchange = cached_exchange, .read = cached_read,
            .exchange_window = cached_exchange_window};
        kms_cached_update_start(&cached_update, &io, image, slot, (q->flags & KMS_M_FORCE) != 0, q->id);
        kms_put_u32(r->data, cached_update.token); r->length = 4; r->status = KMS_M_OK;
    } else if (q->op == KMS_M_CACHE_PROGRESS && q->length == 4) {
        if (kms_get_u32(q->data) != cached_update.token || !cached_update.state) { r->status = KMS_M_EXPIRED; return; }
        const uint32_t values[] = {cached_update.token, cached_update.state, cached_update.result,
            cached_update.offset, cached_update.image.image_bytes, cached_update.slot};
        for (unsigned i = 0; i < 6; ++i) kms_put_u32(r->data + i * 4, values[i]);
        r->length = 24; r->status = KMS_M_OK;
    } else if (q->op >= KMS_M_CACHE_INFO && q->op <= KMS_M_CACHE_ABORT) {
        if (q->op != KMS_M_CACHE_INFO && kms_cached_update_busy(&cached_update)) r->status = KMS_M_BUSY;
        else kms_receiver_cache_request(q, r);
    }
}
static void print_identity(unsigned slot, const kms_firmware_identity_t *i, uint32_t age)
{
    char sha[65]; kms_hex_encode(i->sha256, 32, sha);
    ESP_LOGI(TAG, "Slave %u git=%s UTC=%s bytes=%" PRIu32 " SHA-256=%s confirmed=%s identity-age=%" PRIu32 "ms",
        slot, i->git, i->utc, i->image_bytes, sha, i->capabilities & 4 ? "yes" : "no", age);
}
static kms_management_frame_t make_request(unsigned slot, unsigned op)
{
    kms_management_frame_t request = {.slot = (uint8_t)slot, .op = (uint8_t)op, .id = next_id()};
    return request;
}
static void automatic_restart(const job_t *job)
{
    unsigned result = KMS_M_BUSY; peer_view_t p = peer_view(job->slot);
    bool confirmed;
    unsigned selected = kms_link_selected();
    bool blocked = !kms_remote_recovery_enabled() || kms_remote_transfer_busy() || kms_link_pairing() ||
        kms_radio_channel_recovering() || kms_test_mode_active() ||
        (history_active && !kms_time_elapsed(kms_now(), history_activity, 5000u)) ||
        (!job->frame.flags && selected == job->slot) || !(p.flags & KMS_FLAG_USB_READY);
    if (!blocked && p.boot == job->frame.target &&
        kms_remote_recovery_supported(job->slot, job->frame.target, &confirmed) && confirmed) {
        kms_recovery_budget_t budgets[KMS_SLOTS], requested;
        memcpy(&requested, job->frame.data, sizeof(requested));
        portENTER_CRITICAL(&state_lock); memcpy(budgets, recovery_budgets, sizeof(budgets)); portEXIT_CRITICAL(&state_lock);
        budgets[job->slot - 1] = requested;
        if (memcmp(requested.mac, p.mac, 6)) result = KMS_M_WRONG_BOOT;
        else if (!recovery_save(true, budgets)) result = KMS_M_STORAGE;
        else {
            portENTER_CRITICAL(&state_lock); memcpy(recovery_budgets, budgets, sizeof(budgets)); portEXIT_CRITICAL(&state_lock);
            kms_management_frame_t q, r = {0};
            if (p.identity.capabilities & KMS_CAP_RADIO_DIAGNOSTICS) {
                q = make_request(job->slot, KMS_M_RADIO_STATUS); q.target = job->frame.target;
                unsigned diagnostics = rpc_attempts(&q, &r, 1);
                if (diagnostics == KMS_M_OK && r.length == KMS_RADIO_STATUS_WORDS * 4u)
                    ESP_LOGI(TAG, "KMSRECOVERY evidence slot=%u boot=%08" PRIx32 " failed=%" PRIu32
                        " max_tx_ms=%" PRIu32 " rx_gap_ms=%" PRIu32 " dispatch_ms=%" PRIu32,
                        job->slot, q.target, kms_get_u32(r.data + 20), kms_get_u32(r.data + 32),
                        kms_get_u32(r.data + 52), kms_get_u32(r.data + 56));
            }
            q = make_request(job->slot, KMS_M_RESTART); q.target = job->frame.target;
            kms_put_u32(q.data, KMS_RESTART_KEY); q.length = 4;
            if (p.identity.capabilities & KMS_CAP_RADIO_DIAGNOSTICS) {
                kms_put_u32(q.data + 4, KMS_RESTART_AUTOMATIC); q.length = 8;
            }
            portENTER_CRITICAL(&state_lock);
            selected = kms_link_selected();
            automatic_id = q.id; automatic_until = kms_now() + KMS_MANAGEMENT_RETRY_MS * KMS_MANAGEMENT_ATTEMPTS;
            automatic_active = job->frame.flags != 0; automatic_sending = true; automatic_dispatched = false;
            portEXIT_CRITICAL(&state_lock);
            if (!kms_remote_recovery_enabled() || kms_remote_transfer_busy() || kms_link_pairing() ||
                kms_radio_channel_recovering() || kms_test_mode_active() || (!job->frame.flags && selected == job->slot))
                result = KMS_M_BUSY;
            else result = rpc(&q, &r);
            portENTER_CRITICAL(&state_lock);
            if (result == KMS_M_TIMEOUT && !automatic_dispatched) result = KMS_M_BUSY;
            automatic_sending = false;
            portEXIT_CRITICAL(&state_lock);
            ESP_LOGW(TAG, "KMSRECOVERY restart slot=%u boot=%08" PRIx32 " id=%08" PRIx32
                " active=%u attempt=%u level=%u result=%s", job->slot, q.target, q.id,
                job->frame.flags != 0, requested.attempts, requested.level, kms_management_result_name(result));
        }
    }
    portENTER_CRITICAL(&state_lock);
    recovery_result = (kms_recovery_result_t){.slot = job->slot, .boot = job->frame.target, .status = result};
    recovery_result_pending = true;
    portEXIT_CRITICAL(&state_lock);
}
static void configure_recovery(const job_t *job)
{
    bool enabled = job->frame.flags != 0;
    if (recovery_save(enabled, recovery_budgets)) {
        portENTER_CRITICAL(&state_lock); recovery_enabled = enabled; portEXIT_CRITICAL(&state_lock);
        printf("KMSRECOVERY CONFIG %s\n", enabled ? "ON" : "OFF");
    } else { printf("KMSRECOVERY CONFIG STORAGE_ERROR\n"); }
}
static void list_peers(void)
{
    for (unsigned slot = 1; slot <= KMS_SLOTS; ++slot) {
        peer_view_t p = peer_view(slot);
        if (!p.paired) continue;
        ESP_LOGI(TAG, "Slave %u paired boot=%08" PRIx32 " USB=%s last-contact=%" PRIu32 "ms",
            slot, p.boot, p.flags & KMS_FLAG_USB_READY ? "ready" : "offline", kms_now() - p.seen);
        // Stable machine-readable membership, independent of diagnostic support.
        printf("KMSPEER %u %08" PRIx32 "\n", slot, p.boot);
        printf("KMSPEERUSB %u %02X%02X%02X%02X%02X%02X\n", slot,
               p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
        if (p.identity_seen) print_identity(slot, &p.identity, kms_now() - p.identity_seen);
        else ESP_LOGI(TAG, "Slave %u firmware identity pending/unsupported", slot);
        if (p.summary_seen) ESP_LOGI(TAG, "Slave %u health: RSSI=%" PRId32 " USB=%" PRIu32
            " events=%" PRIu32 "/%" PRIu32 " overwritten=%" PRIu32 " summary-age=%" PRIu32 "ms",
            slot, (int32_t)kms_get_u32(p.summary + 31 * 4), kms_get_u32(p.summary + 14 * 4),
            kms_get_u32(p.summary + 34 * 4), kms_get_u32(p.summary + 35 * 4),
            kms_get_u32(p.summary + 36 * 4), kms_now() - p.summary_seen);
    }
    portENTER_CRITICAL(&state_lock); unsigned selected = own_slot; portEXIT_CRITICAL(&state_lock);
    printf("KMSROUTE CURRENT %u\n", selected);
    printf("KMSPEERS END\n");
}
static void console_job(const job_t *job)
{
    if (job->type == LIST) { list_peers(); return; }
    kms_management_frame_t q = job->type == RAW ? job->frame :
        make_request(job->slot, job->type == CLEAR ? KMS_M_HISTORY_CLEAR :
                     job->type == HISTORY ? KMS_M_HISTORY_BEGIN : KMS_M_IDENTITY);
    kms_management_frame_t r = {0};
    if (q.op == KMS_M_HISTORY_BEGIN || q.op == KMS_M_HISTORY_READ) {
        history_active = true; history_activity = kms_now();
    }
    if (job->type == RAW && q.op == KMS_M_OTA_WRITE && q.length > 4) { stream_forward(&q); return; }
    unsigned result = rpc(&q, &r);
    if (job->type == RAW) {
        observe_upload(&q, result);
        if ((q.op == KMS_M_OTA_BEGIN || q.op == KMS_M_OTA_ABORT) &&
            (result == KMS_M_OK || result == KMS_M_CURRENT)) stream_reset(q.slot);
    }
    if (q.op == KMS_M_RESTART)
        ESP_LOGI(TAG, "KMSRESTART slot=%u boot=%08" PRIx32 " id=%08" PRIx32 " result=%s",
            q.slot, q.target, q.id, kms_management_result_name(result));
    if (job->type == RAW) {
        if (result == KMS_M_TIMEOUT || result == KMS_M_UNSUPPORTED || !r.id) {
            r = q; r.flags = KMS_M_REPLY; r.status = (uint8_t)result; r.length = 0;
            r.sender = q.target; r.target = boot_id;
        }
        uint8_t wire[KMS_M_MAX_WIRE]; char hex[KMS_M_MAX_WIRE * 2 + 1];
        size_t bytes = kms_management_encode(&r, wire, sizeof(wire));
        kms_hex_encode(wire, bytes, hex); printf("KMSM %u %s\n", q.slot, hex); return;
    }
    if (result != KMS_M_OK) {
        ESP_LOGW(TAG, "Slave %u: %s", job->slot, kms_management_result_name(result)); return;
    }
    if (job->type == CLEAR) { ESP_LOGI(TAG, "Slave %u history cleared and saved", job->slot); return; }
    if (job->type == STATUS) {
        kms_firmware_identity_t identity;
        if (kms_identity_decode(&identity, r.data, r.length)) print_identity(job->slot, &identity, 0);
        q = make_request(job->slot, KMS_M_STATS);
        result = rpc(&q, &r);
        if (result == KMS_M_OK) kms_slave_diagnostics_print_summary(r.data, r.length);
        else ESP_LOGW(TAG, "Slave %u summary: %s", job->slot, kms_management_result_name(result));
        return;
    }
    if (job->type == HISTORY && r.length == 12) {
        uint32_t token = kms_get_u32(r.data), size = kms_get_u32(r.data + 4), crc = kms_get_u32(r.data + 8);
        if (!size || size > KMS_SLAVE_HISTORY_BYTES) { ESP_LOGW(TAG, "Invalid snapshot size"); return; }
        // Validate once, then stream the same immutable snapshot. No full-text
        // allocation on either board. END is emitted only after a verified pass.
        for (unsigned pass = 0; pass < 2; ++pass) {
            uint32_t offset = 0, actual_crc = 0;
            if (pass) printf("KMSHISTORY %u BEGIN\n", job->slot);
            while (offset < size) {
                q = make_request(job->slot, KMS_M_HISTORY_READ); q.offset = offset;
                q.length = 4; kms_put_u32(q.data, token); result = rpc(&q, &r);
                if (result != KMS_M_OK || !r.length || r.length > size - offset || r.offset != offset) break;
                actual_crc = kms_crc32_extend(actual_crc, r.data, r.length);
                if (pass) { fwrite(r.data, 1, r.length, stdout); vTaskDelay(pdMS_TO_TICKS(1)); }
                offset += r.length;
            }
            if (offset != size || actual_crc != crc) {
                ESP_LOGW(TAG, "Slave %u history incomplete or corrupt; retry download", job->slot); break;
            }
            if (pass) printf("KMSHISTORY %u END\n", job->slot);
        }
        q = make_request(job->slot, KMS_M_HISTORY_READ); q.offset = size;
        q.length = 4; kms_put_u32(q.data, token); (void)rpc(&q, &r);
    }
}
void kms_remote_desktop(unsigned source, uint32_t boot, kms_desktop_message_t *m)
{
    if (!master || !jobs || !desktop_results || !source || source > KMS_SLOTS) { m->result = KMS_D_UNSUPPORTED; return; }
    unsigned i = source - 1;
    uint32_t token = kms_get_u32(m->data), offset = kms_get_u32(m->data + 4);
    if (m->op == KMS_D_REMOTE_READ) {
        portENTER_CRITICAL(&state_lock);
        if (desktop_results[i].boot != boot || desktop_results[i].session != m->session ||
            desktop_results[i].id != token || kms_time_elapsed(kms_now(), desktop_results[i].at, 30000)) m->result = KMS_D_STALE;
        else if (desktop_results[i].busy) m->result = KMS_D_BUSY;
        else {
            const kms_management_frame_t *r = &desktop_results[i].reply;
            if (offset > r->length || offset % 32) m->result = KMS_D_INVALID;
            else {
                memset(m->data, 0, sizeof(m->data));
                kms_put_u32(m->data, r->status); kms_put_u32(m->data + 4, r->length);
                kms_put_u32(m->data + 8, r->sender); kms_put_u32(m->data + 12, offset);
                unsigned n = r->length - offset; if (n > 32) n = 32;
                memcpy(m->data + 16, r->data + offset, n);
            }
        }
        portEXIT_CRITICAL(&state_lock); return;
    }
    unsigned slot = m->data[0], op = m->data[1], length = m->data[2];
    bool allowed = op == KMS_M_IDENTITY || op == KMS_M_STATS || op == KMS_M_HISTORY_BEGIN ||
        op == KMS_M_HISTORY_READ || op == KMS_M_HISTORY_CLEAR || op == KMS_M_RADIO_STATUS;
    if (!slot || slot > KMS_SLOTS || !allowed || length > 4 || m->data[3] ||
        (op == KMS_M_HISTORY_READ ? length != 4 : length != 0)) { m->result = KMS_D_INVALID; return; }
    if (kms_remote_transfer_busy() || kms_radio_channel_changing() || kms_link_pairing()) { m->result = KMS_D_BUSY; return; }
    job_t job = {.type = DESKTOP_REMOTE, .slot = slot, .desktop_source = source,
        .desktop_boot = boot, .desktop_session = m->session, .desktop_id = m->id};
    job.frame.op = (uint8_t)op; job.frame.slot = (uint8_t)slot; job.frame.offset = offset;
    job.frame.target = kms_get_u32(m->data + 8); job.frame.length = (uint16_t)length;
    memcpy(job.frame.data, m->data + 12, length);
    portENTER_CRITICAL(&state_lock);
    bool duplicate = desktop_results[i].boot == boot && desktop_results[i].session == m->session && desktop_results[i].id == m->id;
    if (!duplicate) {
        if (desktop_results[i].busy) m->result = KMS_D_BUSY;
        else {
            desktop_results[i].boot = boot; desktop_results[i].session = m->session;
            desktop_results[i].id = m->id; desktop_results[i].at = kms_now(); desktop_results[i].busy = true;
        }
    }
    portEXIT_CRITICAL(&state_lock);
    if (!m->result && !duplicate && xQueueSend(jobs, &job, 0) != pdTRUE) {
        portENTER_CRITICAL(&state_lock); desktop_results[i].busy = false; desktop_results[i].id = 0; portEXIT_CRITICAL(&state_lock);
        m->result = KMS_D_BUSY;
    }
    memset(m->data, 0, sizeof(m->data)); kms_put_u32(m->data, m->id);
}
static void desktop_remote_job(const job_t *job)
{
    kms_management_frame_t q = job->frame, reply = {0}; q.id = next_id();
    reply.status = (uint8_t)((kms_remote_transfer_busy() || kms_radio_channel_changing() || kms_link_pairing())
        ? KMS_M_BUSY : rpc(&q, &reply));
    unsigned i = job->desktop_source - 1;
    portENTER_CRITICAL(&state_lock);
    if (desktop_results[i].boot == job->desktop_boot && desktop_results[i].session == job->desktop_session &&
        desktop_results[i].id == job->desktop_id) {
        desktop_results[i].reply = reply; desktop_results[i].busy = false; desktop_results[i].at = kms_now();
    }
    portEXIT_CRITICAL(&state_lock);
}
static void master_worker(void *arg)
{
    (void)arg; job_t job;
    while (true) {
        if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (job.type == DESKTOP_REMOTE) desktop_remote_job(&job);
            else if (job.type == AUTO_RESTART) automatic_restart(&job);
            else if (job.type == AUTO_CONFIG) configure_recovery(&job);
            else if (job.type == LOCAL) {
                local_request(&job.frame);
                portENTER_CRITICAL(&state_lock);
                if (job.frame.op == KMS_M_OTA_BEGIN && local_begins_queued) --local_begins_queued;
                portEXIT_CRITICAL(&state_lock);
            } else console_job(&job);
            if (job.type == RAW && job.frame.op == KMS_M_RESTART) {
                uint32_t now = kms_now();
                portENTER_CRITICAL(&state_lock);
                --manual_restart_jobs;
                manual_restart_wait = true;
                manual_restart_at = now;
                portEXIT_CRITICAL(&state_lock);
            }
            if (changes_firmware(&job)) {
                portENTER_CRITICAL(&state_lock);
                if (transfer_jobs_queued) --transfer_jobs_queued;
                portEXIT_CRITICAL(&state_lock);
            }
        }
        portENTER_CRITICAL(&state_lock);
        bool alive = !kms_time_elapsed(kms_now(), app_tick, 250), usb = usb_initialized;
        portEXIT_CRITICAL(&state_lock);
        kms_ota_tick(alive && (kms_build_identity()->capabilities & 1), usb, local_seen);
        kms_receiver_cache_tick(kms_now());
        kms_cached_update_step(&cached_update, kms_now());
        portENTER_CRITICAL(&state_lock);
        cache_transfer_busy = kms_receiver_cache_busy() || kms_cached_update_busy(&cached_update);
        portEXIT_CRITICAL(&state_lock);
        kms_management_frame_t f;
        while (xQueueReceive(incoming, &f, 0) == pdTRUE) accept_summary(&f);
    }
}
static bool same_request(const kms_management_frame_t *a, const kms_management_frame_t *b)
{
    return a->sender == b->sender && a->target == b->target && a->id == b->id &&
        a->op == b->op && a->slot == b->slot && a->flags == b->flags && a->offset == b->offset &&
        a->length == b->length && !memcmp(a->data, b->data, a->length);
}
static void slave_request(const kms_management_frame_t *q)
{
    if (q->flags & (KMS_M_REPLY | KMS_M_SUMMARY)) return;
    for (unsigned i = 0; cache && i < REPLY_CACHE_SIZE; ++i)
        if (cache[i].valid && !kms_time_elapsed(kms_now(), cache[i].at, KMS_MANAGEMENT_SESSION_MS) &&
            same_request(&cache[i].request, q)) { send_frame(&cache[i].reply); return; }
    kms_management_frame_t r = {.op = q->op, .slot = q->slot, .id = q->id, .offset = q->offset,
        .sender = boot_id, .target = q->sender, .flags = KMS_M_REPLY};
    if (q->target != boot_id && !(q->target == 0 && q->op == KMS_M_IDENTITY)) r.status = KMS_M_WRONG_BOOT;
    else if (q->op == KMS_M_IDENTITY && !q->length) {
        kms_firmware_identity_t identity = *kms_build_identity();
        if (kms_ota_confirmed()) identity.capabilities |= 4;
        kms_identity_encode(&identity, r.data); r.length = KMS_IDENTITY_BYTES;
    } else if (q->op == KMS_M_RESTART) {
        bool busy = kms_ota_busy() || !kms_ota_confirmed() || (q->length == 8 && !kms_link_receiver_host_available());
        portENTER_CRITICAL(&state_lock);
        bool was_pending = restart.pending;
        r.status = (uint8_t)kms_restart_request(&restart, q, boot_id, busy, kms_now());
        if (r.status == KMS_M_OK && !was_pending) restart_saved = false;
        portEXIT_CRITICAL(&state_lock);
        if (r.status == KMS_M_OK && !was_pending) {
            free(snapshot); snapshot = NULL; snapshot_size = snapshot_token = 0;
            bool saved = kms_slave_diagnostics_before_restart(q->length == 8);
            portENTER_CRITICAL(&state_lock);
            if (!saved && q->length == 8) { restart.pending = false; r.status = KMS_M_STORAGE; }
            else { restart.requested_at = kms_now(); restart_saved = true; }
            portEXIT_CRITICAL(&state_lock);
            ESP_LOGI(TAG, "Restart evidence persisted=%u", saved);
        }
        ESP_LOGI(TAG, "Restart request controller=%08" PRIx32 " id=%08" PRIx32 " boot=%08" PRIx32 " result=%s delay_ms=%u",
            q->sender, q->id, boot_id, kms_management_result_name(r.status), KMS_RESTART_DELAY_MS);
    } else if (kms_remote_restart_pending() && q->op != KMS_M_IDENTITY && q->op != KMS_M_STATS) {
        r.status = KMS_M_BUSY;
    } else if (q->op == KMS_M_STATS && !q->length) {
        r.length = (uint16_t)kms_slave_diagnostics_summary(r.data);
    } else if (q->op == KMS_M_STREAM_STATUS && q->length <= 1) {
        if (q->length && !kms_stream_set_profile(q->data[0])) r.status = KMS_M_BAD_REQUEST;
        else r.length = (uint16_t)kms_stream_radio_status(r.data, sizeof(r.data));
    } else if (q->op == KMS_M_RADIO_STATUS && !q->length) {
        r.length = (uint16_t)kms_link_radio_status(r.data, sizeof(r.data));
    } else if (q->op == KMS_M_HISTORY_BEGIN && !q->length) {
        if (kms_slave_diagnostics_local_active()) { r.status = KMS_M_BUSY; send_frame(&r); return; }
        if (!snapshot) snapshot = malloc(sizeof(*snapshot));
        if (!snapshot) r.status = KMS_M_BUSY;
        else {
            snapshot_size = kms_slave_diagnostics_snapshot(snapshot) ? snapshot->size : 0;
            if (!snapshot_size) {
                free(snapshot); snapshot = NULL; snapshot_token = 0; r.status = KMS_M_STORAGE;
            }
            else {
                snapshot_token = q->id; snapshot_owner = q->sender; snapshot_at = kms_now();
                kms_put_u32(r.data, snapshot_token); kms_put_u32(r.data + 4, snapshot_size);
                kms_put_u32(r.data + 8, snapshot->crc); r.length = 12;
            }
        }
    } else if (q->op == KMS_M_HISTORY_READ && q->length == 4) {
        if (!snapshot || snapshot_owner != q->sender || kms_get_u32(q->data) != snapshot_token ||
            kms_time_elapsed(kms_now(), snapshot_at, KMS_MANAGEMENT_SESSION_MS)) r.status = KMS_M_EXPIRED;
        else if (q->offset == snapshot_size) {
            free(snapshot); snapshot = NULL; snapshot_size = snapshot_token = 0;
        } else if (q->offset > snapshot_size) r.status = KMS_M_OFFSET;
        else {
            uint32_t bytes = snapshot_size - q->offset;
            if (bytes > KMS_M_DATA) bytes = KMS_M_DATA;
            r.length = (uint16_t)kms_slave_snapshot_read(snapshot, q->offset, r.data, bytes); snapshot_at = kms_now();
        }
    } else if (q->op == KMS_M_HISTORY_CLEAR && !q->length) {
        free(snapshot); snapshot = NULL; snapshot_size = snapshot_token = 0;
        for (unsigned i = 0; cache && i < REPLY_CACHE_SIZE; ++i)
            if (cache[i].request.op == KMS_M_HISTORY_BEGIN || cache[i].request.op == KMS_M_HISTORY_READ)
                cache[i].valid = false;
        if (!kms_slave_diagnostics_clear()) r.status = KMS_M_STORAGE;
    } else if (q->op >= KMS_M_OTA_BEGIN && q->op <= KMS_M_OTA_ABORT) {
        portENTER_CRITICAL(&state_lock); bool active = input_active; portEXIT_CRITICAL(&state_lock);
        if (q->op == KMS_M_OTA_BEGIN && active) r.status = KMS_M_BUSY;
        else kms_ota_request(q, &r);
    }
    else r.status = KMS_M_BAD_REQUEST;
    if (r.status != KMS_M_BUSY) {
        cached_reply_t *saved = &cache[cache_next++ % REPLY_CACHE_SIZE];
        saved->request = *q; saved->reply = r; saved->at = kms_now(); saved->valid = true;
    }
    send_frame(&r);
}
static void slave_worker(void *arg)
{
    (void)arg;
    while (true) {
        kms_desktop_message_t request;
        if (xQueueReceive(jobs, &request, 0) == pdTRUE) {
            kms_desktop_message_t reply;
            if (kms_remote_restart_pending() || snapshot) { reply = request; reply.result = KMS_D_BUSY; }
            else kms_slave_diagnostics_local(&request, &reply);
            kms_desktop_local_reply(&reply);
        }
        kms_management_frame_t q;
        if (xQueueReceive(incoming, &q, pdMS_TO_TICKS(20)) == pdTRUE) slave_request(&q);
        kms_slave_diagnostics_maintain();
        kms_jiggler_store_maintain();
        portENTER_CRITICAL(&state_lock);
        bool alive = !kms_time_elapsed(kms_now(), app_tick, 250), usb = usb_initialized, radio = radio_seen;
        unsigned slot = own_slot;
        portEXIT_CRITICAL(&state_lock);
        kms_ota_tick(alive && (kms_build_identity()->capabilities & 1), usb, radio);
        if (snapshot && kms_time_elapsed(kms_now(), snapshot_at, KMS_MANAGEMENT_SESSION_MS)) {
            free(snapshot); snapshot = NULL; snapshot_size = 0;
        }
        if (slot && kms_time_elapsed(kms_now(), last_summary, KMS_MANAGEMENT_SUMMARY_MS)) {
            q = make_request(slot, KMS_M_IDENTITY); q.sender = boot_id; q.flags = KMS_M_SUMMARY;
            kms_firmware_identity_t identity = *kms_build_identity();
            if (kms_ota_confirmed()) identity.capabilities |= 4;
            kms_identity_encode(&identity, q.data); q.length = KMS_IDENTITY_BYTES; send_frame(&q);
            q.op = KMS_M_STATS; q.id = next_id(); q.length = (uint16_t)kms_slave_diagnostics_summary(q.data);
            send_frame(&q); last_summary = kms_now();
        }
    }
}
bool kms_remote_local_diagnostics(const kms_desktop_message_t *q)
{
    if (master || !jobs) return false;
    return xQueueSend(jobs, q, 0) == pdTRUE;
}
void kms_remote_init(bool is_master, uint32_t boot)
{
    configASSERT(is_master == master);
    boot_id = boot; sequence = esp_random();
    if (!kms_build_init()) ESP_LOGW(TAG, "Running image identity unavailable; inspect firmware before updates");
    if (!master) kms_slave_diagnostics_init(boot);
    kms_ota_init(master);
    if (master) {
        kms_receiver_cache_init(); recovery_load();
        desktop_results = kms_memory_calloc(KMS_SLOTS, sizeof(*desktop_results));
    }
    jobs = kms_memory_queue_create(KMS_MANAGEMENT_QUEUE_DEPTH, master ? sizeof(job_t) : sizeof(kms_desktop_message_t));
    incoming = kms_memory_queue_create(KMS_MANAGEMENT_QUEUE_DEPTH, sizeof(kms_management_frame_t));
    if (master) outgoing = kms_memory_queue_create(1, sizeof(outbound_t));
    cache = kms_memory_calloc(REPLY_CACHE_SIZE, sizeof(*cache));
    if (!jobs || !incoming || (master && !outgoing) || !cache ||
        xTaskCreate(master ? master_worker : slave_worker, "kms-management", 8192, NULL, 1, NULL) != pdPASS) {
        kms_memory_queue_delete(jobs);
        kms_memory_queue_delete(incoming);
        kms_memory_queue_delete(outgoing);
        jobs = incoming = outgoing = NULL;
        free(cache); cache = NULL;
        ESP_LOGW(TAG, "Management worker unavailable; input continues");
        kms_ota_startup_failed();
    }
}
void kms_remote_tick(unsigned slot, bool usb_ok, bool active)
{
    uint32_t now = kms_now();
    portENTER_CRITICAL(&state_lock);
    own_slot = slot; app_tick = now; usb_initialized = usb_ok; input_active = active;
    portEXIT_CRITICAL(&state_lock);
    if (!master) kms_slave_diagnostics_tick();
    else {
        bool busy = kms_remote_transfer_busy();
        if (busy != transfer_was_busy) {
            portENTER_CRITICAL(&state_lock);
            unsigned queued = transfer_jobs_queued; bool caching = cache_transfer_busy;
            uint8_t uploads = 0;
            for (unsigned i = 0; i < KMS_SLOTS; ++i)
                if (kms_remote_upload_busy(&remote_uploads[i], peers[i].boot, now)) uploads |= (uint8_t)(1u << i);
            portEXIT_CRITICAL(&state_lock);
            ESP_LOGI(TAG, "KMSTRANSFER busy=%u queued=%u cache=%u remote_uploads=0x%02x master=%u",
                busy, queued, caching, uploads, kms_remote_master_updating());
            transfer_was_busy = busy;
        }
    }
}
bool kms_remote_console(const char *line)
{
    if (!master) return false;
    if (!strcmp(line, "receiver-recovery")) {
        printf("KMSRECOVERY enabled=%u inactive=%u active=%u backoff_ms=%u max_backoff_ms=%u attempts_per_hour=%u test_deferred=%u\n",
            kms_remote_recovery_enabled(), KMS_RECOVERY_IDLE_FAILURES, KMS_RECOVERY_ACTIVE_FAILURES,
            KMS_RECOVERY_BACKOFF_MS, KMS_RECOVERY_BACKOFF_MAX_MS, KMS_RECOVERY_MAX_ATTEMPTS, kms_test_mode_active());
        for (unsigned i = 0; i < KMS_SLOTS; ++i) {
            peer_view_t peer = peer_view(i + 1); if (!peer.paired) continue;
            kms_recovery_budget_t budget; kms_remote_recovery_budget(i + 1, peer.mac, &budget);
            printf("KMSRECOVERY BUDGET slot=%u attempts=%u level=%u last_boot=%08" PRIx32 " reasons=0x%" PRIx32 "\n",
                i + 1, budget.attempts, budget.level, budget.boot, budget.reasons);
        }
        return true;
    }
    if (!strcmp(line, "receiver-recovery off") || !strcmp(line, "receiver-recovery on")) {
        bool enabled = !strcmp(line, "receiver-recovery on");
        if (!enabled) { portENTER_CRITICAL(&state_lock); recovery_enabled = false; portEXIT_CRITICAL(&state_lock); }
        job_t job = {.type = AUTO_CONFIG}; job.frame.flags = enabled;
        if (!jobs || xQueueSend(jobs, &job, 0) != pdTRUE) printf("KMSRECOVERY CONFIG BUSY\n");
        return true;
    }
    if (!strcmp(line, "transfer-status")) {
        printf("KMSTRANSFER busy=%u\n", kms_remote_transfer_busy()); return true;
    }
    job_t job = {0}; unsigned slot; int consumed = 0;
    if (!strncmp(line, "master ", 7)) {
        uint8_t wire[KMS_M_MAX_WIRE]; size_t bytes;
        if (!kms_hex_decode(line + 7, wire, sizeof(wire), &bytes) ||
            !kms_management_decode(&job.frame, wire, bytes) || job.frame.slot != 1 ||
            job.frame.flags & (KMS_M_REPLY | KMS_M_SUMMARY) || job.frame.status) {
            ESP_LOGW(TAG, "Malformed local master request"); return true;
        }
        job.type = LOCAL; job.slot = 1;
        if (job.frame.op == KMS_M_OTA_BEGIN && (kms_link_selected() || kms_link_pairing() || kms_radio_channel_changing())) {
            kms_management_frame_t reply = job.frame;
            reply.flags = KMS_M_REPLY; reply.status = KMS_M_BUSY; reply.sender = boot_id;
            reply.target = job.frame.sender; reply.length = 0; print_local(&reply); return true;
        }
    } else if (!strcmp(line, "slaves")) job.type = LIST;
    else if (sscanf(line, "slave-status %u%n", &slot, &consumed) == 1 && !line[consumed]) {
        job.type = STATUS; job.slot = slot;
    } else if (sscanf(line, "slave-history %u%n", &slot, &consumed) == 1 && !line[consumed]) {
        job.type = HISTORY; job.slot = slot;
    } else if (sscanf(line, "slave-history-clear %u%n", &slot, &consumed) == 1 && !line[consumed]) {
        job.type = CLEAR; job.slot = slot;
    } else if (sscanf(line, "remote %u %n", &slot, &consumed) == 1 && consumed > 0) {
        uint8_t wire[KMS_M_MAX_WIRE]; size_t bytes;
        if (!kms_hex_decode(line + consumed, wire, sizeof(wire), &bytes) ||
            !kms_management_decode(&job.frame, wire, bytes) || job.frame.slot != slot ||
            job.frame.flags & (KMS_M_REPLY | KMS_M_SUMMARY) || job.frame.status) {
            ESP_LOGW(TAG, "Malformed management request"); return true;
        }
        job.type = RAW; job.slot = slot;
    } else return false;
    if (job.type != LIST && (!job.slot || job.slot > KMS_SLOTS)) {
        ESP_LOGW(TAG, "Slave slot must be 1-%u", KMS_SLOTS); return true;
    }
    bool restart_request = job.type == RAW && job.frame.op == KMS_M_RESTART;
    if (restart_request && (kms_radio_channel_recovering() || kms_link_receiver_restart_in_progress())) {
        kms_management_frame_t reply = job.frame;
        reply.sender = job.frame.target; reply.target = boot_id;
        reply.flags = KMS_M_REPLY; reply.status = KMS_M_BUSY; reply.length = 0;
        print_remote(&reply); return true;
    }
    if (kms_radio_channel_changing()) {
        ESP_LOGW(TAG, "Radio channel change in progress; retry management shortly"); return true;
    }
    // Reserve before enqueueing: flash work remains protected while queued and
    // executing. Read-only cache inspection/progress must not start a lease.
    bool transfer = changes_firmware(&job);
    if (transfer) { portENTER_CRITICAL(&state_lock); ++transfer_jobs_queued; portEXIT_CRITICAL(&state_lock); }
    if (restart_request) { portENTER_CRITICAL(&state_lock); ++manual_restart_jobs; portEXIT_CRITICAL(&state_lock); }
    bool reserve = job.type == LOCAL && job.frame.op == KMS_M_OTA_BEGIN;
    if (reserve) { portENTER_CRITICAL(&state_lock); ++local_begins_queued; portEXIT_CRITICAL(&state_lock); }
    if (!jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
        if (transfer) { portENTER_CRITICAL(&state_lock); --transfer_jobs_queued; portEXIT_CRITICAL(&state_lock); }
        if (restart_request) { portENTER_CRITICAL(&state_lock); --manual_restart_jobs; portEXIT_CRITICAL(&state_lock); }
        if (reserve) { portENTER_CRITICAL(&state_lock); if (local_begins_queued) --local_begins_queued; portEXIT_CRITICAL(&state_lock); }
        ESP_LOGW(TAG, "Management busy/unavailable; retry shortly");
    }
    return true;
}
