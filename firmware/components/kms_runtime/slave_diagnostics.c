#include "kms_slave_diagnostics.h"
#include "kms_runtime.h"
#include "sdkconfig.h"
#include "kms_build.h"
#include "kms_jiggler_store.h"
#include "kms_test_mode.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "slave-history";

#if !CONFIG_IDF_TARGET_ESP32S3
// Legacy single-blob layout, used only when importing an existing journal.
typedef struct { kms_slave_journal_t journal; uint32_t crc; } stored_journal_t;
typedef struct { uint32_t version, bank, bytes, crc; } journal_head_t;
enum { JOURNAL_VERSION = 1, JOURNAL_CHUNK = 2048,
       JOURNAL_CHUNKS = (sizeof(kms_slave_journal_t) + JOURNAL_CHUNK - 1) / JOURNAL_CHUNK };
enum save_result { SAVE_OK, SAVE_RETRY, SAVE_ERROR };
_Static_assert(JOURNAL_CHUNKS <= 10, "Journal keys have one digit for the chunk number");
_Static_assert(sizeof(stored_journal_t) <= offsetof(kms_slave_history_t, live) + 2 * sizeof(uint32_t),
               "Legacy import may use the live boot and reset fields as scratch");
static kms_slave_history_t history;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool initialized, storage_ready, dirty, urgent;
static uint32_t generation, last_save;
static uint32_t radio_fault_at[KMS_RF_REASONS];
static bool radio_fault_seen[KMS_RF_REASONS];
static uint32_t radio_checkpoint_at;
static bool radio_checkpoint_seen;
static bool storing, store_seen;
static uint32_t store_finished;
static kms_slave_event_record_t *local_events;
static uint32_t local_owner, local_token, local_at, local_info[12];
bool kms_slave_diagnostics_local_active(void) { return local_token != 0; }
static void close_local_history(void)
{
    free(local_events); local_events = NULL; local_owner = local_token = 0;
}
void kms_slave_diagnostics_local(const kms_desktop_message_t *q, kms_desktop_message_t *r)
{
    *r = *q; memset(r->data, 0, sizeof(r->data)); r->result = KMS_D_INVALID;
    if (!initialized || !q->id || !q->session) return;
    uint32_t now = kms_now();
    if (local_token && kms_time_elapsed(now, local_at, 20000u)) close_local_history();
    if (q->op == KMS_D_LOG_BEGIN) {
        if (local_owner != q->session || local_token != q->id) {
            close_local_history();
            local_events = malloc(sizeof(*local_events) * KMS_SLAVE_HISTORY_EVENTS);
            if (!local_events) { r->result = KMS_D_BUSY; return; }
            portENTER_CRITICAL(&lock);
            const kms_slave_journal_t *j = &history.journal;
            unsigned first = (j->next_event + KMS_SLAVE_HISTORY_EVENTS - j->event_count) % KMS_SLAVE_HISTORY_EVENTS;
            for (unsigned i = 0; i < j->event_count; ++i)
                local_events[i] = j->events[(first + i) % KMS_SLAVE_HISTORY_EVENTS];
            local_info[0] = 1; local_info[1] = history.live.boot; local_info[2] = q->id;
            local_info[3] = j->event_count; local_info[4] = KMS_SLAVE_HISTORY_EVENTS;
            local_info[5] = j->events_overwritten; local_info[6] = j->stats_count; local_info[7] = j->stats_overwritten;
            local_info[8] = (uint32_t)history.live.uptime_ms; local_info[9] = (uint32_t)(history.live.uptime_ms >> 32);
            local_info[10] = history.live.reset; local_info[11] = 0;
            portEXIT_CRITICAL(&lock);
            local_token = q->id; local_owner = q->session;
        }
        for (unsigned i = 0; i < 12; ++i) kms_put_u32(r->data + i * 4, local_info[i]);
    } else {
        if (!local_token || local_owner != q->session || kms_get_u32(q->data) != local_token ||
            kms_get_u32(q->data + 8) != local_info[1]) { r->result = KMS_D_STALE; return; }
        unsigned index = kms_get_u32(q->data + 4);
        if (q->op == KMS_D_LOG_END) close_local_history();
        else if (q->op == KMS_D_LOG_READ && index < local_info[3]) {
            const kms_slave_event_record_t *e = &local_events[index];
            const uint32_t values[] = {1, local_info[1], local_token, index, e->sequence, e->boot,
                e->first_ms, e->last_ms, e->detail, e->repeats, e->event, local_info[3]};
            for (unsigned i = 0; i < 12; ++i) kms_put_u32(r->data + i * 4, values[i]);
        } else return;
    }
    local_at = now; r->result = KMS_D_OK;
}
void kms_slave_diagnostics_fault(unsigned reason, unsigned op, uint32_t duration, uint32_t failed)
{
    if (!initialized || !reason || reason >= KMS_RF_REASONS) return;
    uint32_t now = kms_now();
    portENTER_CRITICAL(&lock);
    history.live.radio_failed = failed;
    bool own_flash = storing || (store_seen && !kms_time_elapsed(now, store_finished, 100u));
    if (own_flash && reason != KMS_RF_MAC_FAILED) { portEXIT_CRITICAL(&lock); return; }
    if (!radio_fault_seen[reason] || kms_time_elapsed(now, radio_fault_at[reason], 5000u)) {
        bool first = !radio_fault_seen[reason];
        radio_fault_seen[reason] = true; radio_fault_at[reason] = now;
        kms_slave_history_event(&history, now, KMS_S_RADIO_ERROR,
            kms_slave_radio_detail(reason, op | (own_flash ? 128u : 0), duration));
        // Freeze surrounding radio/USB/heap counters. Callbacks change RAM only;
        // the management worker coalesces and persists the journal later.
        if (first || !radio_checkpoint_seen || kms_time_elapsed(now, radio_checkpoint_at, 30000u)) {
            kms_slave_history_checkpoint(&history); radio_checkpoint_at = now; radio_checkpoint_seen = true;
        }
        dirty = true; urgent |= first && !own_flash; ++generation;
    }
    portEXIT_CRITICAL(&lock);
}
void kms_slave_diagnostics_event(unsigned event, uint32_t detail)
{
    if (!initialized) return;
    portENTER_CRITICAL(&lock);
    uint32_t now = kms_now();
    bool own_flash = storing || (store_seen && !kms_time_elapsed(now, store_finished, 100u));
    urgent |= kms_slave_history_event(&history, now, event, detail) && !own_flash;
    dirty = true; ++generation;
    portEXIT_CRITICAL(&lock);
}
void kms_slave_diagnostics_add(unsigned metric, uint32_t amount)
{
    if (!initialized || metric >= KMS_S_METRIC_COUNT) return;
    portENTER_CRITICAL(&lock); history.live.metrics[metric] += amount; portEXIT_CRITICAL(&lock);
}
void kms_slave_diagnostics_set(unsigned metric, uint32_t value)
{
    if (!initialized || metric >= KMS_S_METRIC_COUNT) return;
    portENTER_CRITICAL(&lock); history.live.metrics[metric] = value; portEXIT_CRITICAL(&lock);
}
void kms_slave_diagnostics_peak(unsigned metric, uint32_t value)
{
    if (!initialized || metric >= KMS_S_METRIC_COUNT) return;
    portENTER_CRITICAL(&lock);
    if (value > history.live.metrics[metric]) history.live.metrics[metric] = value;
    portEXIT_CRITICAL(&lock);
}
void kms_slave_diagnostics_radio(int rssi, unsigned depth)
{
    if (!initialized) return;
    portENTER_CRITICAL(&lock);
    ++history.live.metrics[KMS_S_RX]; history.live.last_radio_ms = kms_now();
    history.live.rssi_last = rssi;
    if (history.rssi_samples == UINT32_MAX) { history.rssi_sum /= 2; history.rssi_samples /= 2; }
    history.rssi_sum += rssi; ++history.rssi_samples;
    history.live.rssi_mean = (int32_t)(history.rssi_sum / history.rssi_samples);
    if (depth > history.live.metrics[KMS_S_PEAK_RX]) history.live.metrics[KMS_S_PEAK_RX] = depth;
    portEXIT_CRITICAL(&lock);
}
static void storage_error(void)
{ kms_slave_diagnostics_event(KMS_S_STORAGE_ERROR, 0); }

static size_t chunk_size(unsigned index)
{
    size_t left = sizeof(history.journal) - index * JOURNAL_CHUNK;
    return left < JOURNAL_CHUNK ? left : JOURNAL_CHUNK;
}
static void chunk_key(char key[11], unsigned bank, unsigned index)
{
    memcpy(key, "journal0_0", 11);
    key[7] = (char)('0' + bank);
    key[9] = (char)('0' + index);
}
static bool valid_head(const journal_head_t *head, size_t bytes)
{
    return bytes == sizeof(*head) && head->version == JOURNAL_VERSION &&
           head->bank < 2 && head->bytes == sizeof(history.journal);
}
static bool load_journal(nvs_handle_t h)
{
    journal_head_t head;
    size_t bytes = sizeof(head);
    esp_err_t result = nvs_get_blob(h, "journal_head", &head, &bytes);
    uint32_t crc;
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        // At boot there are no readers. Import in place and restore the live
        // boot/reset fields after validating the old blob.
        bytes = sizeof(stored_journal_t);
        if (nvs_get_blob(h, "journal", &history, &bytes) != ESP_OK || bytes != sizeof(stored_journal_t))
            return false;
        memcpy(&crc, (const uint8_t *)&history + offsetof(stored_journal_t, crc), sizeof(crc));
    } else {
        if (result != ESP_OK || !valid_head(&head, bytes)) return false;
        for (unsigned i = 0; i < JOURNAL_CHUNKS; ++i) {
            char key[11]; chunk_key(key, head.bank, i);
            bytes = chunk_size(i);
            if (nvs_get_blob(h, key, (uint8_t *)&history.journal + i * JOURNAL_CHUNK, &bytes) != ESP_OK ||
                bytes != chunk_size(i)) return false;
        }
        crc = head.crc;
    }
    return crc == kms_crc32(&history.journal, sizeof(history.journal)) &&
           kms_slave_history_valid(&history.journal);
}
void kms_slave_diagnostics_init(uint32_t boot)
{
    uint32_t now = kms_now(), reset = (uint32_t)esp_reset_reason();
    kms_slave_history_init(&history, now, boot, reset);
    storage_ready = nvs_flash_init_partition("kms_diag") == ESP_OK;
    nvs_handle_t h;
    if (storage_ready && nvs_open_from_partition("kms_diag", "history", NVS_READONLY, &h) == ESP_OK) {
        if (load_journal(h)) { history.live.boot = boot; history.live.reset = reset; }
        else kms_slave_history_init(&history, now, boot, reset);
        nvs_close(h);
    }
    initialized = true;
    if (!storage_ready) {
        ESP_LOGW(TAG, "History partition unavailable; RAM diagnostics continue"); storage_error();
    }
    kms_slave_diagnostics_event(KMS_S_BOOT, history.live.reset);
}
void kms_slave_diagnostics_tick(void)
{
    if (!initialized) return;
    uint32_t now = kms_now();
    portENTER_CRITICAL(&lock);
    kms_slave_history_tick(&history, now);
    if (kms_time_elapsed(now, history.checkpoint, KMS_SLAVE_HISTORY_STATS_MS)) {
        kms_slave_history_checkpoint(&history); history.checkpoint = now;
        dirty = urgent = true; ++generation;
    }
    portEXIT_CRITICAL(&lock);
}
static enum save_result stage_journal(nvs_handle_t h, uint8_t *scratch, uint32_t revision)
{
    journal_head_t old;
    size_t bytes = sizeof(old);
    esp_err_t result = nvs_get_blob(h, "journal_head", &old, &bytes);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND && result != ESP_ERR_NVS_INVALID_LENGTH)
        return SAVE_ERROR;
    journal_head_t head = {.version = JOURNAL_VERSION, .bytes = sizeof(history.journal)};
    if (result == ESP_OK && valid_head(&old, bytes)) head.bank = old.bank ^ 1u;
    // The old bank stays readable until all chunks of a consistent revision
    // are durable and the single NVS head key is replaced.
    for (unsigned i = 0; i < JOURNAL_CHUNKS; ++i) {
        bytes = chunk_size(i);
        portENTER_CRITICAL(&lock);
        bool current = generation == revision;
        if (current) memcpy(scratch, (const uint8_t *)&history.journal + i * JOURNAL_CHUNK, bytes);
        portEXIT_CRITICAL(&lock);
        if (!current) return SAVE_RETRY;
        head.crc = kms_crc32_extend(head.crc, scratch, bytes);
        char key[11]; chunk_key(key, head.bank, i);
        if (nvs_set_blob(h, key, scratch, bytes) != ESP_OK) return SAVE_ERROR;
    }
    if (nvs_commit(h) != ESP_OK || nvs_set_blob(h, "journal_head", &head, sizeof(head)) != ESP_OK ||
        nvs_commit(h) != ESP_OK) return SAVE_ERROR;
    return SAVE_OK;
}
static enum save_result save_current(bool clear_legacy)
{
    if (!storage_ready) return SAVE_ERROR;
    uint8_t *scratch = malloc(JOURNAL_CHUNK);
    if (!scratch) return SAVE_ERROR;
    portENTER_CRITICAL(&lock);
    uint32_t revision = generation; storing = true;
    portEXIT_CRITICAL(&lock);
    nvs_handle_t h;
    enum save_result result = SAVE_ERROR;
    if (nvs_open_from_partition("kms_diag", "history", NVS_READWRITE, &h) == ESP_OK) {
        result = stage_journal(h, scratch, revision);
        if (result == SAVE_OK && clear_legacy) {
            // An explicit clear also retires the copy read by older firmware.
            esp_err_t erased = nvs_erase_key(h, "journal");
            if (erased != ESP_ERR_NVS_NOT_FOUND && (erased != ESP_OK || nvs_commit(h) != ESP_OK))
                result = SAVE_ERROR;
        }
        nvs_close(h);
    }
    free(scratch);
    portENTER_CRITICAL(&lock);
    storing = false; store_seen = true; store_finished = kms_now();
    if (result == SAVE_OK && revision == generation) dirty = false;
    urgent = result == SAVE_RETRY || (result == SAVE_OK && dirty && urgent);
    last_save = store_finished;
    portEXIT_CRITICAL(&lock);
    return result;
}
bool kms_slave_diagnostics_before_restart(bool automatic)
{
    if (!initialized) return false;
    close_local_history();
    portENTER_CRITICAL(&lock);
    kms_slave_history_event(&history, kms_now(), KMS_S_RADIO_ERROR,
        kms_slave_radio_detail(automatic ? KMS_RF_AUTO_RESTART : KMS_RF_RESTART, 0, 0));
    kms_slave_history_checkpoint(&history); dirty = urgent = true; ++generation;
    portEXIT_CRITICAL(&lock);
    enum save_result result = save_current(false);
    if (result == SAVE_RETRY) result = save_current(false);
    bool settings_saved = kms_jiggler_store_flush();
    if (result == SAVE_ERROR || !settings_saved) storage_error();
    return result == SAVE_OK && settings_saved;
}
void kms_slave_diagnostics_maintain(void)
{
    if (!initialized) return;
    if (local_token && kms_time_elapsed(kms_now(), local_at, 20000u)) close_local_history();
    uint32_t heap = esp_get_minimum_free_heap_size();
    portENTER_CRITICAL(&lock);
    history.live.metrics[KMS_S_MIN_HEAP] = heap;
    bool save = dirty && kms_time_elapsed(kms_now(), last_save,
        urgent ? KMS_SLAVE_HISTORY_SAVE_MS : KMS_SLAVE_HISTORY_REPEAT_MS);
    if (save) { urgent = false; last_save = kms_now(); }
    portEXIT_CRITICAL(&lock);
    if (save && kms_test_mode_history_allowed(kms_now()) && save_current(false) == SAVE_ERROR) storage_error();
}
bool kms_slave_diagnostics_clear(void)
{
    // This worker serializes every NVS write. It never queues old journal copies,
    // so a pending flush cannot resurrect records after a successful clear.
    if (!initialized) return false;
    close_local_history();
    portENTER_CRITICAL(&lock);
    kms_slave_history_clear(&history); dirty = urgent = true; ++generation;
    portEXIT_CRITICAL(&lock);
    if (!storage_ready) {
        // Explicit clear is allowed to repair only the dedicated history partition.
        storage_ready = nvs_flash_erase_partition("kms_diag") == ESP_OK &&
                        nvs_flash_init_partition("kms_diag") == ESP_OK;
    }
    enum save_result result = save_current(true);
    if (result == SAVE_RETRY) result = save_current(true);
    if (result == SAVE_ERROR) storage_error();
    return result == SAVE_OK;
}
bool kms_slave_diagnostics_snapshot(kms_slave_snapshot_t *snapshot)
{
    if (!initialized) return false;
    portENTER_CRITICAL(&lock); snapshot->history = history; portEXIT_CRITICAL(&lock);
    snapshot->identity = *kms_build_identity();
    return kms_slave_snapshot_prepare(snapshot);
}
size_t kms_slave_diagnostics_summary(uint8_t *out)
{
    _Static_assert(KMS_S_METRIC_COUNT == 11 && KMS_S_EVENT_COUNT == 16, "Update summary wire schema");
    if (!initialized) return 0;
    uint32_t words[42] = {0};
    portENTER_CRITICAL(&lock);
    const kms_slave_stats_t *s = &history.live; const kms_slave_journal_t *j = &history.journal;
    words[0] = s->boot; words[1] = s->reset;
    words[2] = (uint32_t)s->uptime_ms; words[3] = (uint32_t)(s->uptime_ms >> 32);
    memcpy(words + 4, s->metrics, sizeof(s->metrics));
    memcpy(words + 15, s->totals, sizeof(s->totals));
    words[31] = (uint32_t)s->rssi_last; words[32] = (uint32_t)s->rssi_mean;
    words[33] = kms_now() - s->last_radio_ms;
    words[34] = j->event_count; words[35] = KMS_SLAVE_HISTORY_EVENTS; words[36] = j->events_overwritten;
    words[37] = j->stats_count; words[38] = KMS_SLAVE_HISTORY_STATS; words[39] = j->stats_overwritten;
    if (j->event_count) {
        unsigned first = (j->next_event + KMS_SLAVE_HISTORY_EVENTS - j->event_count) % KMS_SLAVE_HISTORY_EVENTS;
        words[40] = j->events[first].boot; words[41] = j->events[first].first_ms;
    }
    portEXIT_CRITICAL(&lock);
    for (unsigned n = 0; n < 42; ++n) kms_put_u32(out + n * 4, words[n]);
    return sizeof(words);
}
#else
// The S3 queries receiver history but owns no receiver journal. Keep its
// shared runtime entry points inert without reserving the S2's history RAM.
void kms_slave_diagnostics_init(uint32_t boot) { (void)boot; }
void kms_slave_diagnostics_event(unsigned event, uint32_t detail) { (void)event; (void)detail; }
void kms_slave_diagnostics_add(unsigned metric, uint32_t amount) { (void)metric; (void)amount; }
void kms_slave_diagnostics_set(unsigned metric, uint32_t value) { (void)metric; (void)value; }
void kms_slave_diagnostics_peak(unsigned metric, uint32_t value) { (void)metric; (void)value; }
void kms_slave_diagnostics_radio(int rssi, unsigned depth) { (void)rssi; (void)depth; }
void kms_slave_diagnostics_fault(unsigned reason, unsigned op, uint32_t duration, uint32_t failed)
{ (void)reason; (void)op; (void)duration; (void)failed; }
bool kms_slave_diagnostics_before_restart(bool automatic) { (void)automatic; return false; }
void kms_slave_diagnostics_tick(void) { }
void kms_slave_diagnostics_maintain(void) { }
bool kms_slave_diagnostics_clear(void) { return false; }
bool kms_slave_diagnostics_snapshot(kms_slave_snapshot_t *snapshot) { (void)snapshot; return false; }
size_t kms_slave_diagnostics_summary(uint8_t *out) { (void)out; return 0; }
void kms_slave_diagnostics_local(const kms_desktop_message_t *q, kms_desktop_message_t *r)
{ *r = *q; r->result = KMS_D_UNSUPPORTED; }
bool kms_slave_diagnostics_local_active(void) { return false; }
#endif

void kms_slave_diagnostics_print_summary(const uint8_t *data, size_t size)
{
    if (size != 168) return;
    uint32_t w[42]; for (unsigned n = 0; n < 42; ++n) w[n] = kms_get_u32(data + n * 4);
    ESP_LOGI(TAG, "boot=%08" PRIx32 " reset=%" PRIu32 " uptime=%" PRIu64 "ms RSSI=%" PRId32
        " mean=%" PRId32 " last-radio=%" PRIu32 "ms USB-flags=%" PRIu32,
        w[0], w[1], ((uint64_t)w[3] << 32) | w[2], (int32_t)w[31], (int32_t)w[32], w[33], w[14]);
    ESP_LOGI(TAG, "RX=%" PRIu32 " TX=%" PRIu32 " duplicate=%" PRIu32 " rejected=%" PRIu32
        " accepted=%" PRIu32 " USB-completed=%" PRIu32 " min-heap=%" PRIu32
        " peak-RX=%" PRIu32 " peak-USB=%" PRIu32 " max-USB=%" PRIu32 "ms",
        w[4], w[5], w[6], w[7], w[8], w[9], w[13], w[10], w[11], w[12]);
    ESP_LOGI(TAG, "Events=%" PRIu32 "/%" PRIu32 " overwritten=%" PRIu32 " checkpoints=%" PRIu32
        "/%" PRIu32 " overwritten=%" PRIu32 " oldest-boot=%08" PRIx32 " oldest-at=%" PRIu32 "ms",
        w[34], w[35], w[36], w[37], w[38], w[39], w[40], w[41]);
    for (unsigned n = 0; n < KMS_S_EVENT_COUNT; ++n)
        ESP_LOGI(TAG, "Total %s: %" PRIu32, kms_slave_event_name(n), w[15 + n]);
}
