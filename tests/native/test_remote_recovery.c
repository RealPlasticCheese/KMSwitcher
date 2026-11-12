#include "kms_core.h"
#include "kms_receiver_recovery.h"
#include "kms_remote.h"
#include "nvs.h"
#include <inttypes.h>
#include <string.h>
#define KMS_NVS_NAMESPACE "kmswitcher"
typedef unsigned QueueHandle_t;
enum { pdTRUE = 1 };
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static void fake_log(const char *format, ...) { (void)format; }
#define ESP_LOGI(tag, ...) fake_log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log(__VA_ARGS__)
#define printf(...) fake_log(__VA_ARGS__)
#include "remote_types.inc"
static unsigned checks, clock_ms, selected_slot, writes, restart_calls, diagnostic_calls, wire_sent, sequence;
static unsigned after_enqueue, after_diagnostics;
static bool master = true, transfer_busy, pairing, changing, testing, fail_storage, fail_open, committed, order_error;
static bool recovery_enabled = true, recovery_reserved, recovery_result_pending, history_active;
static bool automatic_sending, automatic_active, automatic_dispatched;
static uint32_t automatic_id, automatic_until, history_activity, boot_id = 99;
static kms_recovery_result_t recovery_result;
static kms_recovery_budget_t recovery_budgets[KMS_SLOTS];
static peer_view_t fixture_peer;
static QueueHandle_t jobs = 1, outgoing = 2;
static int state_lock;
static uint8_t stored[KMS_RECOVERY_STORE_BYTES];
static bool have_stored;
typedef struct { unsigned head, count; uint8_t data[4][512]; } queue_t;
static queue_t queues[3];
static uint32_t kms_now(void) { return clock_ms; }
static peer_view_t peer_view(unsigned slot) { (void)slot; return fixture_peer; }
static uint32_t next_id(void) { return ++sequence; }
bool kms_remote_transfer_busy(void) { return transfer_busy; }
static bool kms_link_pairing(void) { return pairing; }
static bool kms_radio_channel_recovering(void) { return changing; }
static bool kms_test_mode_active(void) { return testing; }
static unsigned kms_link_selected(void) { return selected_slot; }
static int xQueueSend(QueueHandle_t q, const void *item, unsigned wait)
{
    (void)wait; queue_t *queue = &queues[q]; if (queue->count == 4) return 0;
    memcpy(queue->data[(queue->head + queue->count) % 4], item, q == jobs ? sizeof(job_t) : sizeof(outbound_t));
    ++queue->count; return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q, void *item, unsigned wait)
{
    (void)wait; queue_t *queue = &queues[q]; if (!queue->count) return 0;
    memcpy(item, queue->data[queue->head], q == jobs ? sizeof(job_t) : sizeof(outbound_t));
    queue->head = (queue->head + 1) % 4; --queue->count; return pdTRUE;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{ (void)name; (void)mode; *out = 1; return fail_open ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    (void)h; (void)key; if (!have_stored) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < sizeof(stored)) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, stored, sizeof(stored)); *size = sizeof(stored); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    (void)h; (void)key; if (fail_storage || size != sizeof(stored)) return ESP_FAIL;
    memcpy(stored, data, size); have_stored = true; ++writes; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; committed = !fail_storage; return committed ? ESP_OK : ESP_FAIL; }
static bool kms_stream_enqueue(const kms_management_frame_t *f)
{ outbound_t item = {.frame = *f}; return xQueueSend(outgoing, &item, 0) == pdTRUE; }
#include "remote_send.inc"
#include "remote_take.inc"
#include "remote_budget.inc"
#include "remote_request.inc"
static unsigned rpc_attempts(kms_management_frame_t *q, kms_management_frame_t *r, unsigned attempts)
{
    (void)attempts; ++diagnostic_calls; if (!committed) order_error = true;
    memset(r, 0, sizeof(*r)); r->length = KMS_RADIO_STATUS_WORDS * 4; kms_put_u32(r->data, 1);
    kms_put_u32(r->data + 4, q->target); kms_put_u32(r->data + 20, 12);
    if (after_diagnostics == 1) recovery_enabled = false;
    if (after_diagnostics == 2) selected_slot = 1;
    return KMS_M_OK;
}
static kms_management_frame_t transmitted;
static unsigned rpc(kms_management_frame_t *q, kms_management_frame_t *r)
{
    (void)r; ++restart_calls; if (!committed) order_error = true;
    send_frame(q); send_frame(q);
    switch (after_enqueue) {
    case 1: recovery_enabled = false; break; case 2: pairing = true; break;
    case 3: changing = true; break; case 4: testing = true; break;
    case 5: transfer_busy = true; break; case 6: selected_slot = 1; break;
    case 7: clock_ms = automatic_until; break; default: break;
    case 8: fixture_peer.flags = 0; break; case 9: fixture_peer.boot = 23; break;
    }
    if (kms_remote_take_outgoing(&transmitted)) { ++wire_sent; return KMS_M_OK; }
    return KMS_M_TIMEOUT;
}
#include "remote_auto.inc"
#include "remote_config.inc"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static job_t restart_job(bool active)
{
    job_t job = {.type = AUTO_RESTART, .slot = 1}; job.frame.target = 22; job.frame.flags = active;
    kms_recovery_budget_t budget = {.mac = {2, 3, 4, 5, 6, 7}, .boot = 22, .attempts = 1, .level = 1, .reasons = 1};
    memcpy(job.frame.data, &budget, sizeof(budget)); return job;
}
static void fixture(void)
{
    memset(queues, 0, sizeof(queues)); memset(recovery_budgets, 0, sizeof(recovery_budgets));
    fixture_peer = (peer_view_t){.paired = true, .flags = KMS_FLAG_USB_READY, .boot = 22,
        .seen = 10000, .identity_seen = 10000, .mac = {2, 3, 4, 5, 6, 7}};
    fixture_peer.identity.capabilities = (unsigned)KMS_CAP_RESTART | (unsigned)KMS_CAP_RADIO_DIAGNOSTICS | 4u;
    recovery_enabled = true; automatic_sending = automatic_active = recovery_reserved = recovery_result_pending = false;
    history_active = transfer_busy = pairing = changing = testing = fail_storage = fail_open = committed = order_error = false;
    have_stored = false; clock_ms = 10000; selected_slot = 3;
    writes = restart_calls = diagnostic_calls = wire_sent = after_enqueue = after_diagnostics = 0;
}
__declspec(dllexport) int kms_run_remote_recovery_tests(void)
{
    checks = 0; fixture(); job_t job = restart_job(false); automatic_restart(&job);
    CHECK(writes == 1 && restart_calls == 1 && diagnostic_calls == 1 && wire_sent == 1 && !order_error);
    CHECK(transmitted.op == KMS_M_RESTART && transmitted.target == 22 && transmitted.length == 8);
    CHECK(kms_get_u32(transmitted.data) == KMS_RESTART_KEY && kms_get_u32(transmitted.data + 4) == KMS_RESTART_AUTOMATIC);
    CHECK(!kms_remote_take_outgoing(&transmitted)); // No stale duplicate survives RPC completion.
    kms_recovery_result_t result; CHECK(kms_remote_take_recovery_result(&result) && result.status == KMS_M_OK);
    bool enabled; kms_recovery_budget_t decoded[KMS_SLOTS];
    CHECK(kms_recovery_store_decode(&enabled, decoded, stored, sizeof(stored)) && enabled && decoded[0].attempts == 1);
    recovery_enabled = false; memset(recovery_budgets, 0, sizeof(recovery_budgets)); recovery_load();
    CHECK(recovery_enabled && recovery_budgets[0].level == 1);
    stored[0] ^= 1; recovery_load(); CHECK(!recovery_enabled);
    fixture(); fail_storage = true; automatic_restart(&job);
    CHECK(!restart_calls && recovery_result.status == KMS_M_STORAGE);
    for (unsigned mode = 0; mode < 11; ++mode) {
        fixture();
        switch (mode) {
        case 0: recovery_enabled = false; break; case 1: transfer_busy = true; break;
        case 2: pairing = true; break; case 3: changing = true; break;
        case 4: testing = true; break; case 5: selected_slot = 1; break;
        case 6: fixture_peer.flags = 0; break; case 7: fixture_peer.identity.capabilities &= ~4u; break;
        case 8: fixture_peer.identity.capabilities &= ~KMS_CAP_RESTART; break;
        case 9: fixture_peer.boot = 23; break; default: history_active = true; history_activity = clock_ms; break;
        }
        automatic_restart(&job); CHECK(!restart_calls && !wire_sent);
    }
    for (unsigned mode = 1; mode <= 9; ++mode) {
        fixture(); after_enqueue = mode; automatic_restart(&job);
        CHECK(restart_calls == 1 && !wire_sent && !kms_remote_take_outgoing(&transmitted));
    }
    for (unsigned mode = 1; mode <= 2; ++mode) {
        fixture(); after_diagnostics = mode; automatic_restart(&job); CHECK(!restart_calls);
    }
    fixture(); fixture_peer.identity.capabilities &= ~KMS_CAP_RADIO_DIAGNOSTICS;
    automatic_restart(&job); CHECK(wire_sent == 1 && !diagnostic_calls && transmitted.length == 4);
    fixture(); selected_slot = 0; job = restart_job(true); automatic_restart(&job); CHECK(wire_sent == 1);
    fixture(); kms_recovery_budget_t budget; memcpy(&budget, job.frame.data, sizeof(budget));
    CHECK(kms_remote_recovery_request(1, 22, &budget, true));
    CHECK(!kms_remote_recovery_request(1, 22, &budget, true));
    CHECK(xQueueReceive(jobs, &job, 0) == pdTRUE); automatic_restart(&job);
    CHECK(kms_remote_take_recovery_result(&result) && !recovery_reserved);
    job.type = AUTO_CONFIG; job.frame.flags = false; configure_recovery(&job);
    CHECK(!kms_remote_recovery_enabled());
    CHECK(kms_recovery_store_decode(&enabled, decoded, stored, sizeof(stored)) && !enabled);
    return 0;
}
__declspec(dllexport) unsigned kms_remote_recovery_test_count(void) { return checks; }
