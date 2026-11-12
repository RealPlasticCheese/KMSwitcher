#include "../../firmware/components/kms_runtime/slave_diagnostics.c"
#include <stddef.h>
static bool master, restart_saved, restart_issued, ota_busy, confirmed = true, poll_storage, reboot_during_storage;
static int state_lock;
static kms_restart_t restart;
static uint32_t boot_id = 22, snapshot_size, snapshot_token;
static kms_slave_snapshot_t *snapshot;
static bool kms_ota_busy(void) { return ota_busy; }
static bool kms_ota_confirmed(void) { return confirmed; }
static bool usb_available = true;
bool kms_link_receiver_host_available(void) { return usb_available; }
bool kms_remote_take_restart(void);
#include "remote_restart_gate.inc"
#include "slave_restart.inc"
static unsigned jobs = 1;
static bool local_queued;
static kms_desktop_message_t local_job, local_reply;
static int xQueueSend(unsigned queue, const void *item, unsigned wait)
{
    (void)wait;
    if (queue != jobs || local_queued) return 0;
    memcpy(&local_job, item, sizeof(local_job)); local_queued = true; return pdTRUE;
}
static int xQueueReceive(unsigned queue, void *item, unsigned wait)
{
    (void)wait;
    if (queue != jobs || !local_queued) return 0;
    memcpy(item, &local_job, sizeof(local_job)); local_queued = false; return pdTRUE;
}
static void kms_desktop_local_reply(const kms_desktop_message_t *reply) { local_reply = *reply; }
#include "slave_local_jobs.inc"
static unsigned checks, writes;
static uint32_t clock_ms;
static bool fail_storage, fail_head, mutate_on_write, allocated;
static int fail_chunk = -1;
static size_t largest_allocation;
static union { uint64_t align; uint8_t data[sizeof(stored_journal_t)]; } pool;
static stored_journal_t legacy;
static journal_head_t persisted_head;
static bool have_legacy, have_head;
static uint8_t banks[2][JOURNAL_CHUNKS][JOURNAL_CHUNK];
static size_t bank_bytes[2][JOURNAL_CHUNKS];
static kms_slave_snapshot_t frozen;
bool kms_jiggler_store_flush(void) { return true; }
static const kms_firmware_identity_t identity = {.git = "test", .utc = "2026-10-29T00:00:00Z", .chip = 2};
uint32_t kms_now(void) { return clock_ms; }
unsigned esp_reset_reason(void) { return 1; }
unsigned esp_get_minimum_free_heap_size(void) { return 31000; }
const kms_firmware_identity_t *kms_build_identity(void) { return &identity; }
bool kms_test_mode_history_allowed(uint32_t now) { (void)now; return true; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void *malloc(size_t bytes)
{
    if (bytes > largest_allocation) largest_allocation = bytes;
    if (allocated || bytes > sizeof(pool.data)) return NULL;
    allocated = true; return pool.data;
}
void free(void *pointer) { if (pointer == pool.data) allocated = false; }
esp_err_t nvs_flash_init_partition(const char *name) { (void)name; return ESP_OK; }
esp_err_t nvs_flash_erase_partition(const char *name)
{ (void)name; have_legacy = have_head = false; memset(bank_bytes, 0, sizeof(bank_bytes)); return ESP_OK; }
esp_err_t nvs_open_from_partition(const char *part, const char *name, int mode, nvs_handle_t *out)
{ (void)part; (void)name; (void)mode; *out = 1; return fail_storage ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
static bool key_equal(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    if (!key_equal(key, "journal")) return ESP_FAIL;
    if (!have_legacy) return ESP_ERR_NVS_NOT_FOUND;
    have_legacy = false; return ESP_OK;
}
static bool chunk_index(const char *key, unsigned *bank, unsigned *index)
{
    if (memcmp(key, "journal", 7) || key[7] < '0' || key[7] > '1' || key[8] != '_' ||
        key[9] < '0' || key[9] >= '0' + JOURNAL_CHUNKS || key[10]) return false;
    *bank = (unsigned)(key[7] - '0'); *index = (unsigned)(key[9] - '0'); return true;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *bytes)
{
    (void)h;
    const void *data; size_t size;
    if (key_equal(key, "journal")) {
        if (!have_legacy) return ESP_ERR_NVS_NOT_FOUND;
        data = &legacy; size = sizeof(legacy);
    } else if (key_equal(key, "journal_head")) {
        if (!have_head) return ESP_ERR_NVS_NOT_FOUND;
        data = &persisted_head; size = sizeof(persisted_head);
    } else {
        unsigned bank, index;
        if (!chunk_index(key, &bank, &index) || !bank_bytes[bank][index]) return ESP_ERR_NVS_NOT_FOUND;
        data = banks[bank][index]; size = bank_bytes[bank][index];
    }
    if (*bytes < size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, data, size); *bytes = size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t bytes)
{
    (void)h;
    if (fail_storage) return ESP_FAIL;
    if (key_equal(key, "journal_head")) {
        const journal_head_t *head = data;
        if (fail_head || !valid_head(head, bytes) ||
            head->crc != kms_crc32(banks[head->bank], head->bytes)) return ESP_FAIL;
        persisted_head = *head; have_head = true; ++writes; return ESP_OK;
    }
    unsigned bank, index;
    if (!chunk_index(key, &bank, &index) || bytes != chunk_size(index) || (int)index == fail_chunk)
        return ESP_FAIL;
    memcpy(banks[bank][index], data, bytes); bank_bytes[bank][index] = bytes;
    if (mutate_on_write) {
        mutate_on_write = false;
        kms_slave_diagnostics_event(KMS_S_USB_MOUNT, 55);
    }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (poll_storage) { clock_ms += KMS_RESTART_DELAY_MS + 1; reboot_during_storage |= kms_remote_take_restart(); }
    return fail_storage ? ESP_FAIL : ESP_OK;
}
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_run_slave_diagnostics_tests(void)
{
    checks = writes = 0;
    kms_slave_history_init(&history, 0, 5, 3);
    kms_slave_history_event(&history, 1, KMS_S_USB_MOUNT, 123);
    legacy.journal = history.journal; legacy.crc = kms_crc32(&legacy.journal, sizeof(legacy.journal));
    have_legacy = true;
    clock_ms = 100; kms_slave_diagnostics_init(11);
    CHECK(offsetof(kms_slave_stats_t, radio_failed) == 12 && kms_slave_history_valid(&history.journal));
    CHECK(history.journal.event_count == 2 && history.journal.events[0].boot == 5 &&
        history.live.boot == 11 && history.live.reset == 1 && !largest_allocation);
    unsigned baseline = history.journal.event_count;
    clock_ms = 200; kms_slave_diagnostics_fault(KMS_RF_MAC_FAILED, 3, 175, 1);
    CHECK(history.live.radio_failed == 1 && history.journal.event_count == baseline + 1 && !writes);
    CHECK(history.journal.stats_count == 1);
    clock_ms = 300; kms_slave_diagnostics_fault(KMS_RF_MAC_FAILED, 3, 190, 7);
    CHECK(history.live.radio_failed == 7 && history.journal.event_count == baseline + 1);
    CHECK(history.journal.stats_count == 1 && !writes);
    clock_ms = 1100; kms_slave_diagnostics_maintain(); CHECK(writes == 1);
    clock_ms = 6000; kms_slave_diagnostics_fault(KMS_RF_MAC_FAILED, 3, 250, 14);
    CHECK(history.journal.event_count == baseline + 2 && history.journal.stats_count == 1);
    clock_ms = 6100; kms_slave_diagnostics_fault(KMS_RF_RX_GAP, 0, 1500, 14);
    CHECK(history.journal.stats_count == 2);
    CHECK(kms_slave_diagnostics_before_restart(true) && writes == 2);
    CHECK(persisted_head.crc == kms_crc32(&history.journal, sizeof(history.journal)));
    CHECK(largest_allocation == JOURNAL_CHUNK && !allocated);
    journal_head_t previous = persisted_head;
    unsigned events = history.journal.event_count;
    kms_slave_diagnostics_event(KMS_S_USB_SUSPEND, 1);
    for (int i = 0; i < JOURNAL_CHUNKS; ++i) {
        fail_chunk = i;
        CHECK(save_current(false) == SAVE_ERROR && !memcmp(&previous, &persisted_head, sizeof(previous)) && dirty);
    }
    fail_chunk = -1; fail_head = true;
    CHECK(save_current(false) == SAVE_ERROR && !memcmp(&previous, &persisted_head, sizeof(previous)));
    fail_head = false; mutate_on_write = true;
    CHECK(save_current(false) == SAVE_RETRY && urgent && dirty && !allocated);
    CHECK(!memcmp(&previous, &persisted_head, sizeof(previous)) && writes == 2);
    // A reset before publication loads the previous, complete generation.
    clock_ms = 100; kms_slave_diagnostics_init(22);
    CHECK(history.journal.event_count == events + 1 && history.live.boot == 22 && !history.live.radio_failed);
    bool found = false;
    for (unsigned i = 0; i < KMS_SLAVE_HISTORY_EVENTS; ++i)
        if (history.journal.events[i].boot == 11 && (history.journal.events[i].detail & 0xff000000u) ==
            (KMS_RADIO_FAULT_TAG | (KMS_RF_AUTO_RESTART << 24))) found = true;
    CHECK(found && kms_slave_diagnostics_snapshot(&frozen));
    bool retained = false;
    for (unsigned i = 0; i < KMS_SLAVE_HISTORY_STATS; ++i)
        if (history.journal.snapshots[i].boot == 11 && history.journal.snapshots[i].radio_failed == 14) retained = true;
    CHECK(retained);
    kms_slave_diagnostics_event(KMS_S_USB_RESUME, 7);
    CHECK(save_current(false) == SAVE_OK && persisted_head.bank != previous.bank && writes == 3);
    CHECK(persisted_head.crc == kms_crc32(&history.journal, sizeof(history.journal)) && !dirty);
    fail_storage = true; CHECK(!kms_slave_diagnostics_before_restart(true));
    CHECK(history.live.totals[KMS_S_STORAGE_ERROR] == 1);
    fail_storage = false;
    CHECK(have_legacy && kms_slave_diagnostics_clear() && !have_legacy &&
        !history.journal.event_count && !history.journal.stats_count);
    clock_ms = 20000; storing = true; urgent = false; baseline = history.journal.event_count;
    kms_slave_diagnostics_fault(KMS_RF_MAIN_GAP, 0, 175, 0);
    CHECK(history.journal.event_count == baseline && !urgent);
    memset(radio_fault_seen, 0, sizeof(radio_fault_seen));
    kms_slave_diagnostics_fault(KMS_RF_MAC_FAILED, 3, 175, 1);
    CHECK(history.journal.event_count == baseline + 1 && !urgent && history.live.radio_failed == 1);
    CHECK((history.journal.events[(history.journal.next_event + KMS_SLAVE_HISTORY_EVENTS - 1) % KMS_SLAVE_HISTORY_EVENTS].detail & 0x00800000u) != 0);
    storing = false;
    kms_management_frame_t q = {.op = KMS_M_RESTART, .slot = 1, .id = 42, .sender = 99, .target = 22, .length = 8}, reply;
    kms_put_u32(q.data, KMS_RESTART_KEY); kms_put_u32(q.data + 4, KMS_RESTART_AUTOMATIC);
    usb_available = false; restart_request(&q, &reply);
    CHECK(reply.status == KMS_M_BUSY && !restart.pending);
    usb_available = true;
    poll_storage = true; restart_request(&q, &reply);
    CHECK(reply.status == KMS_M_OK && restart_saved && !reboot_during_storage && restart.pending);
    CHECK(!kms_remote_take_restart());
    uint32_t deadline = restart.requested_at;
    ++clock_ms; restart_request(&q, &reply); CHECK(reply.status == KMS_M_OK && restart.requested_at == deadline);
    clock_ms = deadline + KMS_RESTART_DELAY_MS; CHECK(kms_remote_take_restart() && kms_remote_restart_pending());
    CHECK(!kms_remote_take_restart());
    restart = (kms_restart_t){0}; restart_saved = restart_issued = false; fail_storage = true; ++q.id;
    restart_request(&q, &reply); CHECK(reply.status == KMS_M_STORAGE && !restart.pending && !kms_remote_take_restart());
    q.length = 4; ++q.id; restart_request(&q, &reply);
    CHECK(reply.status == KMS_M_OK && restart_saved && restart.pending);
    clock_ms += KMS_RESTART_DELAY_MS; CHECK(kms_remote_take_restart());
    restart = (kms_restart_t){0}; restart_saved = restart_issued = false; fail_storage = false; ota_busy = true; ++q.id;
    restart_request(&q, &reply); CHECK(reply.status == KMS_M_BUSY && !restart.pending);
    ota_busy = false; confirmed = false; restart_request(&q, &reply);
    CHECK(reply.status == KMS_M_BUSY && !restart.pending);
    // Local USB history is immutable, boot/session bound and does no radio or NVS work.
    kms_slave_history_init(&history, 100, 11, 3);
    kms_slave_history_event(&history, 100, KMS_S_BOOT, 3);
    history.live.boot = 22; kms_slave_history_event(&history, 200, KMS_S_USB_MOUNT, 0);
    kms_desktop_message_t local = {.op = KMS_D_LOG_BEGIN, .id = 77, .session = 123}, result;
    unsigned written = writes;
    allocated = true; kms_slave_diagnostics_local(&local, &result); CHECK(result.result == KMS_D_BUSY);
    allocated = false; kms_slave_diagnostics_local(&local, &result);
    CHECK(result.result == KMS_D_OK && kms_get_u32(result.data) == 1 && kms_get_u32(result.data + 12) == 2 && writes == written);
    for (unsigned i = 0; i < 300; ++i) kms_slave_history_event(&history, 300 + i, KMS_S_RADIO_ERROR, i);
    kms_slave_diagnostics_local(&local, &result); CHECK(kms_get_u32(result.data + 12) == 2);
    local.op = KMS_D_LOG_READ; kms_put_u32(local.data, 77); kms_put_u32(local.data + 8, 22);
    kms_slave_diagnostics_local(&local, &result);
    CHECK(result.result == KMS_D_OK && kms_get_u32(result.data + 20) == 11 && kms_get_u32(result.data + 40) == KMS_S_BOOT);
    ++local.session; kms_slave_diagnostics_local(&local, &result); CHECK(result.result == KMS_D_STALE); --local.session;
    kms_put_u32(local.data + 8, 23); kms_slave_diagnostics_local(&local, &result); CHECK(result.result == KMS_D_STALE);
    kms_put_u32(local.data + 8, 22); kms_put_u32(local.data + 4, 2);
    kms_slave_diagnostics_local(&local, &result); CHECK(result.result == KMS_D_INVALID);
    local.op = KMS_D_LOG_END; kms_slave_diagnostics_local(&local, &result); CHECK(result.result == KMS_D_OK && !allocated);
    local.op = KMS_D_LOG_BEGIN; ++local.id; kms_slave_diagnostics_local(&local, &result);
    CHECK(result.result == KMS_D_OK && kms_get_u32(result.data + 12) == KMS_SLAVE_HISTORY_EVENTS);
    clock_ms += 20001; local.op = KMS_D_LOG_READ; kms_put_u32(local.data, local.id);
    kms_slave_diagnostics_local(&local, &result); CHECK(result.result == KMS_D_STALE && !allocated);
    // The migrated head takes precedence over the old blob, even if corrupt.
    // Falling back to the legacy journal could resurrect cleared history.
    have_legacy = true;
    banks[persisted_head.bank][0][0] ^= 1;
    kms_slave_diagnostics_init(33);
    CHECK(history.journal.event_count == 1 && history.journal.events[0].boot == 33);
    local.op = KMS_D_LOG_BEGIN; ++local.id;
    CHECK(kms_remote_local_diagnostics(&local) && !kms_remote_local_diagnostics(&local));
    service_local_job();
    CHECK(!local_queued && local_reply.result == KMS_D_OK && local_reply.id == local.id &&
        local_reply.session == local.session && kms_get_u32(local_reply.data + 4) == 33);
    local.op = KMS_D_LOG_END;
    kms_put_u32(local.data, local.id); kms_put_u32(local.data + 8, 33);
    CHECK(kms_remote_local_diagnostics(&local)); service_local_job();
    CHECK(local_reply.result == KMS_D_OK && !allocated);
    return 0;
}
__declspec(dllexport) unsigned kms_slave_diagnostics_test_count(void) { return checks; }
