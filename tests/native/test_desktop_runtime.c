// Production USB mailbox/radio adapter with deterministic queue/NVS dependencies.
#include "kms_desktop_runtime.h"
#include "kms_desktop_routes.h"
#include "kms_runtime.h"
#include "kms_cached_update.h"
#include "kms_radio_channel.h"
#include "kms_stream_runtime.h"
#include "nvs.h"
#include "freertos/queue.h"
#include <string.h>
typedef struct { unsigned size, head, count; uint8_t data[8][256]; } fake_queue_t;
static fake_queue_t queues[8];
static unsigned queue_index, runtime_checks;
static uint32_t clock_ms = 1000;
static unsigned task_queues;
static unsigned wakeups;
static unsigned queued_updates, selected_station;
static bool cache_upload_busy;
static kms_cached_update_t cached_firmware;
static kms_remote_upload_t direct_firmware;
static uint8_t stored_config[KMS_D_CONFIG_BYTES], stored_screens[KMS_SLOTS][KMS_D_SCREENS_BYTES];
static uint8_t stored_layout[KMS_D_LAYOUT_BYTES];
static size_t stored_layout_bytes;
static size_t stored_config_bytes, stored_screen_bytes[KMS_SLOTS];
static unsigned screen_writes;
static const uint8_t *psram_config;
static kms_desktop_message_t local_log_request;
bool kms_remote_local_diagnostics(const kms_desktop_message_t *q) { local_log_request = *q; return true; }
bool kms_link_recovery_status(unsigned slot, uint8_t out[48])
{ memset(out, 0, 48); kms_put_u32(out, 1); kms_put_u32(out + 16, slot); return slot && slot <= 7; }
void kms_app_wake(void) { ++wakeups; }
QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{ (void)count; fake_queue_t *q = &queues[queue_index++]; memset(q, 0, sizeof(*q)); q->size = size; return q; }
QueueHandle_t kms_memory_queue_create(unsigned count, unsigned bytes)
{ ++task_queues; return xQueueCreate(count, bytes); }
static uint8_t stream_storage[4096];
static kms_desktop_routes_t route_storage;
static kms_desktop_master_t desktop_storage;
static bool fail_desktop_memory;
void *kms_memory_calloc(size_t count, size_t size)
{
    if (count == 1 && size == sizeof(route_storage)) { memset(&route_storage, 0, sizeof(route_storage)); return &route_storage; }
    if (count == 1 && size == sizeof(desktop_storage)) {
        if (fail_desktop_memory) return NULL;
        memset(&desktop_storage, 0, sizeof(desktop_storage)); return &desktop_storage;
    }
    if (count * size > sizeof(stream_storage)) return NULL;
    memset(stream_storage, 0, sizeof(stream_storage)); return stream_storage;
}
uint32_t kms_stream_local_status(unsigned slot, unsigned incoming)
{ (void)slot; return KMS_STREAM_STATUS_TAG | (incoming << 16) | (kms_stream_capacity() << 8) | 255; }
unsigned kms_stream_radio_source_profile(unsigned slot) { (void)slot; return 0; }
int xQueueSend(QueueHandle_t handle, const void *item, uint32_t wait)
{
    (void)wait; fake_queue_t *q = handle;
    if (q->count == 8 || q->size > 256) return 0;
    memcpy(q->data[(q->head + q->count) % 8], item, q->size); ++q->count; return 1;
}
int xQueueReceive(QueueHandle_t handle, void *item, uint32_t wait)
{
    (void)wait; fake_queue_t *q = handle; if (!q->count) return 0;
    memcpy(item, q->data[q->head], q->size); q->head = (q->head + 1) % 8; --q->count; return 1;
}
int xQueueReset(QueueHandle_t handle) { fake_queue_t *q = handle; q->head = q->count = 0; return 1; }
unsigned uxQueueMessagesWaiting(QueueHandle_t handle) { return ((fake_queue_t *)handle)->count; }
uint32_t kms_now(void) { return clock_ms; }
bool kms_link_select(void *ctx, unsigned slot) { (void)ctx; selected_station = slot; return true; }
bool kms_link_desktop_available(unsigned slot) { (void)slot; return true; }
bool kms_link_pairing(void) { return false; }
bool kms_remote_transfer_busy(void)
{ return queued_updates || cache_upload_busy || kms_cached_update_busy(&cached_firmware) ||
    kms_remote_upload_busy(&direct_firmware, 20, clock_ms); }
bool kms_radio_channel_changing(void) { return false; }
static unsigned channel_sets, channel_resumes, recovery_commands;
static const char *channel_error;
static kms_radio_channel_status_t radio_status = {6, 6, 1, 13, 2, 2};
void kms_radio_channel_snapshot(kms_radio_channel_status_t *out) { *out = radio_status; }
const char *kms_radio_channel_set(unsigned channel)
{ (void)channel; ++channel_sets; return channel_error; }
void kms_link_resume(void) { ++channel_resumes; }
static bool channel_recovery_command(unsigned slot, uint32_t boot, kms_desktop_message_t *m)
{
    (void)slot; (void)boot;
    ++recovery_commands;
    if (m->op == KMS_D_CHANNEL_RECOVERY_GET) m->data[2] = 15;
    return true;
}
unsigned kms_link_selected(void) { return 1; }
bool kms_test_mode_active(void) { return false; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) { (void)name; (void)mode; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *n)
{
    (void)h; const uint8_t *value = stored_config; size_t size = stored_config_bytes;
    if (key[4] == '_') { value = stored_layout; size = stored_layout_bytes; }
    else if (key[7] == '_' && key[8] == 's' && key[9] >= '1' && key[9] <= '7') {
        unsigned i = (unsigned)(key[9] - '1'); value = stored_screens[i]; size = stored_screen_bytes[i];
    }
    if (!size) return ESP_ERR_NVS_NOT_FOUND;
    if (*n < size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(data, value, size); *n = size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t n)
{
    (void)h;
    if (key[4] == '_' && n == sizeof(stored_layout)) {
        if (data == psram_config) return ESP_FAIL;
        memcpy(stored_layout, data, n); stored_layout_bytes = n; return ESP_OK;
    }
    if (key[7] == '\0' && n == sizeof(stored_config)) {
        if (data == psram_config) return ESP_FAIL;
        memcpy(stored_config, data, n); stored_config_bytes = n; return ESP_OK;
    }
    if (key[7] == '_' && key[8] == 's' && key[9] >= '1' && key[9] <= '7' && n == KMS_D_SCREENS_BYTES) {
        unsigned i = (unsigned)(key[9] - '1'); memcpy(stored_screens[i], data, n);
        stored_screen_bytes[i] = n; ++screen_writes; return ESP_OK;
    }
    return ESP_FAIL;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
#include "../../firmware/components/kms_runtime/desktop_runtime.c"
static bool take_clipboard(kms_management_frame_t *frame)
{
    unsigned indices[4]; kms_stream_batch_t batch = {0};
    unsigned n = kms_stream_select(false, indices, &batch, 1);
    if (!n) n = kms_stream_select(true, indices, &batch, 1);
    if (!n) return false;
    *frame = batch.frames[0]; kms_stream_commit(indices, n); return true;
}
#define VERIFY(c) do { ++runtime_checks; if (!(c)) return __LINE__; } while (0)
static void usb_command(kms_desktop_message_t *q)
{ uint8_t data[64]; kms_desktop_encode(q, data); kms_desktop_usb_set(data, sizeof(data)); }
static kms_desktop_message_t usb_status(void)
{
    uint8_t data[64]; kms_desktop_message_t m = {0};
    kms_desktop_usb_get(KMS_D_STATUS_REPORT, data, sizeof(data)); kms_desktop_decode(&m, data, sizeof(data)); return m;
}
static int post_update_handover(void);
static int channel_controls(void);
static int device_controls(void);
__declspec(dllexport) int kms_run_desktop_runtime_tests(void)
{
    runtime_checks = queue_index = wakeups = 0;
    kms_stream_runtime_init(); kms_desktop_init(false, 42); kms_desktop_link_tick(2, 99, 7, false, true, true);
    VERIFY(task_queues == 3 && !state && ((fake_queue_t *)outgoing)->size < sizeof(kms_management_frame_t));
    kms_desktop_message_t s = usb_status();
    VERIFY((s.id & 0xff000000u) == KMS_STREAM_STATUS_TAG && ((s.id >> 8) & 255u) == 4);
    VERIFY(s.data[0] == 2 && kms_get_u32(s.data + 4) == 42);
    kms_desktop_message_t q = {.op = KMS_D_HELLO, .id = 1, .session = 11};
    kms_management_frame_t f;
    usb_command(&q); VERIFY(wakeups == 1 && !kms_desktop_take_outgoing(&f));
    kms_desktop_link_tick(2, 99, 7, false, true, true);
    VERIFY(kms_desktop_take_outgoing(&f) && f.slot == 2 && f.sender == 42 && !f.target &&
        f.op == KMS_M_DESKTOP && f.id == q.id && f.length == KMS_D_BYTES && !f.offset && !f.status);
    f.flags = KMS_M_REPLY; f.sender = 100; f.target = 42;
    kms_desktop_receive(&f); kms_desktop_link_tick(2, 99, 7, false, true, true);
    s = usb_status(); VERIFY(s.session == 11 && kms_get_u32(s.data + 8) == 100);
    // The inactive input guard still knows controller 99. Discovery must keep 100.
    q.op = KMS_D_HEARTBEAT; q.id = 2; usb_command(&q);
    kms_desktop_link_tick(2, 99, 7, false, true, true);
    VERIFY(kms_desktop_take_outgoing(&f) && f.target == 100);
    kms_desktop_usb_reset(); s = usb_status(); VERIFY(!s.session && !kms_get_u32(s.data + 24));
    q = (kms_desktop_message_t){.op = KMS_D_HELLO, .id = 3, .session = 22}; usb_command(&q);
    kms_desktop_link_tick(2, 99, 7, false, true, true);
    VERIFY(kms_desktop_take_outgoing(&f) && !f.target);
    kms_desktop_message_t old = {.op = KMS_D_HELLO, .id = 1, .session = 11};
    f.flags = KMS_M_REPLY; f.sender = 100; f.target = 42; kms_desktop_encode(&old, f.data);
    kms_desktop_receive(&f); VERIFY(master_boot == 0);
    f.sender = 200; kms_desktop_encode(&q, f.data); kms_desktop_receive(&f); VERIFY(master_boot == 200);
    q = (kms_desktop_message_t){.op = KMS_D_PREPARE, .id = 77, .session = 22};
    kms_put_u32(q.data, 10); kms_put_u32(q.data + 4, 20); kms_put_u32(q.data + 8, 3);
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    s = usb_status(); VERIFY(kms_get_u32(s.data + 24) == 77 && kms_get_u32(s.data + 28) == 10 && kms_get_u32(s.data + 36) == 3);
    q = (kms_desktop_message_t){.op = KMS_D_READY, .id = 4, .session = 22}; kms_put_u32(q.data, 77); usb_command(&q);
    kms_desktop_link_tick(2, 99, 7, false, true, true);
    s = usb_status(); VERIFY(!kms_get_u32(s.data + 24));
    kms_sample_t input = {.x = 5, .state = {.modifiers = 1}}; kms_desktop_input(&input);
    s = usb_status(); VERIFY(s.data[2] && kms_get_u32(s.data + 12) == 1 && kms_get_u32(s.data + 16) == 5);
    input = (kms_sample_t){0}; kms_desktop_input(&input);
    s = usb_status(); VERIFY(!s.data[2] && kms_get_u32(s.data + 12) == 1);
    clock_ms += 1000; s = usb_status(); VERIFY(!kms_get_u32(s.data + 24));
    kms_clipboard_packet_t p = {.kind = KMS_C_DATA_CHUNK, .source = 2, .target = 1,
        .source_session = 22, .target_session = 11, .sequence = 5, .transfer = 6, .length = 160};
    uint8_t clipboard[KMS_C_BYTES]; memset(p.data, 0xa5, sizeof(p.data));
    VERIFY(kms_clipboard_encode(&p, clipboard)); kms_clipboard_usb_set(clipboard, sizeof(clipboard));
    kms_desktop_link_tick(2, 200, 7, false, true, true);
    VERIFY(take_clipboard(&f) && f.op == KMS_M_CLIPBOARD && f.slot == 2 && f.target == 200);
    p.source = 1; p.target = 2; p.source_session = 11; p.target_session = 22;
    kms_clipboard_encode(&p, f.data); f.sender = 200; f.target = 42; f.flags = KMS_M_REPLY;
    kms_clipboard_receive(&f);
    s = usb_status(); VERIFY(((s.id >> 16) & 255u) == 1);
    VERIFY(kms_clipboard_usb_get(clipboard, sizeof(clipboard)) == KMS_C_BYTES);
    VERIFY(kms_clipboard_decode(&p, clipboard, sizeof(clipboard)) && p.length == 160 && p.data[159] == 0xa5);
    s = usb_status(); VERIFY(((s.id >> 16) & 255u) == 0);
    f.sender = 199; kms_clipboard_receive(&f); kms_clipboard_usb_get(clipboard, sizeof(clipboard));
    VERIFY(!kms_clipboard_decode(&p, clipboard, sizeof(clipboard)));
    fail_desktop_memory = true; kms_desktop_init(true, 42);
    kms_desktop_motion(1, 0); kms_desktop_link_tick(2, 200, 7, false, true, true);
    VERIFY(!state);
    fail_desktop_memory = false; queue_index = 0; kms_desktop_init(true, 42);
    VERIFY(state == &desktop_storage);
    kms_desktop_block(true); kms_desktop_link_tick(2, 200, 7, false, true, true);
    VERIFY(!state->idle);
    kms_desktop_block(false); kms_desktop_link_tick(2, 200, 7, false, true, true);
    VERIFY(state->idle);
    state->peers[0] = (kms_desktop_peer_t){.session = 11, .boot = 100, .seen = clock_ms};
    state->peers[1] = (kms_desktop_peer_t){.session = 22, .boot = 200, .seen = clock_ms};
    p = (kms_clipboard_packet_t){.kind = KMS_C_QUERY, .source = 1, .target = 2,
        .source_session = 11, .target_session = 22, .sequence = 1};
    kms_clipboard_encode(&p, f.data); f.slot = 1; f.sender = 100; f.target = 42; f.flags = 0;
    kms_clipboard_receive(&f);
    VERIFY(take_clipboard(&f) && f.slot == 2 && f.target == 200 && f.sender == 42 && f.flags == KMS_M_REPLY);
    f.slot = 1; f.sender = 100; f.target = 42; f.flags = 0;
    state->peers[1].session = 23; kms_clipboard_receive(&f);
    VERIFY(!take_clipboard(&f));
    is_master = false;
    while (kms_desktop_take_outgoing(&f)) { }
    q = (kms_desktop_message_t){.op = KMS_D_HEARTBEAT, .id = 90, .session = 22};
    memset(clipboard, 0, sizeof(clipboard)); kms_desktop_encode(&q, clipboard);
    kms_desktop_usb_set(clipboard, sizeof(clipboard));
    kms_desktop_link_tick(2, 200, 7, false, true, true);
    VERIFY(kms_desktop_take_outgoing(&f) && f.op == KMS_M_DESKTOP && f.id == 90);
    clipboard[100] = 1; kms_desktop_usb_set(clipboard, sizeof(clipboard));
    kms_desktop_link_tick(2, 200, 7, false, true, true);
    VERIFY(!kms_desktop_take_outgoing(&f));
    unsigned before = wakeups;
    kms_desktop_usb_set(clipboard, 1); VERIFY(wakeups == before);
    // Full command queues do not wake the application for discarded requests.
    for (unsigned i = 0; i < 8; ++i) usb_command(&q);
    VERIFY(wakeups == before + 8);
    usb_command(&q); VERIFY(wakeups == before + 8);
    // Diagnostics distinguish stale sessions, relay progress and bounded queue
    // saturation without retaining any clipboard payload.
    kms_clipboard_diagnostics_t diagnostics; kms_clipboard_diagnostics(&diagnostics);
    VERIFY(diagnostics.usb_received == 1 && diagnostics.radio_received == 4);
    VERIFY(diagnostics.unauthorized == 2 && diagnostics.relayed == 1 && diagnostics.delivered == 1);
    VERIFY(!diagnostics.queue_drops);
    for (unsigned i = 0; i < 9; ++i) kms_clipboard_usb_set(clipboard, 0);
    kms_clipboard_diagnostics(&diagnostics); VERIFY(diagnostics.usb_received == 1);
    p.source = 2; p.target = 1; p.source_session = 22; p.target_session = 11;
    kms_clipboard_encode(&p, clipboard);
    for (unsigned i = 0; i < 9; ++i) kms_clipboard_usb_set(clipboard, sizeof(clipboard));
    kms_clipboard_diagnostics(&diagnostics);
    VERIFY(diagnostics.usb_received == 10 && diagnostics.commands == 0 && diagnostics.outgoing == 6 && diagnostics.queue_drops == 3);
    int failure = post_update_handover(); if (failure) return failure;
    is_master = false; xQueueReset(commands);
    while (kms_desktop_take_outgoing(&f)) { }
    q = (kms_desktop_message_t){.op = KMS_D_LOCAL_HELLO, .id = 200, .session = 456};
    usb_command(&q); kms_desktop_link_tick(0, 0, 0, false, true, true);
    uint8_t report[64]; kms_desktop_message_t answer;
    kms_desktop_usb_get(KMS_D_COMMAND_REPORT, report, sizeof(report)); kms_desktop_decode(&answer, report, sizeof(report));
    VERIFY(answer.op == KMS_D_LOCAL_HELLO && answer.session == 456 && !answer.result && !kms_desktop_take_outgoing(&f));
    q.op = KMS_D_LOG_BEGIN; ++q.id; usb_command(&q); kms_desktop_link_tick(0, 0, 0, false, true, true);
    VERIFY(local_log_request.op == KMS_D_LOG_BEGIN && local_log_request.id == q.id && !kms_desktop_take_outgoing(&f));
    answer = q; kms_put_u32(answer.data, 1); kms_desktop_local_reply(&answer);
    kms_desktop_usb_get(KMS_D_COMMAND_REPORT, report, sizeof(report)); kms_desktop_decode(&answer, report, sizeof(report));
    VERIFY(answer.op == KMS_D_LOG_BEGIN && kms_get_u32(answer.data) == 1);
    answer.session = 455; answer.result = KMS_D_STORAGE; kms_desktop_local_reply(&answer);
    kms_desktop_usb_get(KMS_D_COMMAND_REPORT, report, sizeof(report)); kms_desktop_decode(&answer, report, sizeof(report));
    VERIFY(answer.session == 456 && !answer.result);
    q.session = 455; q.id = 202; usb_command(&q); kms_desktop_link_tick(0, 0, 0, false, true, true);
    VERIFY(local_log_request.id == 201);
    kms_desktop_usb_reset(); kms_desktop_local_reply(&answer);
    kms_desktop_usb_get(KMS_D_COMMAND_REPORT, report, sizeof(report)); kms_desktop_decode(&answer, report, sizeof(report));
    VERIFY(!answer.session);
    // Persist the reported inside-corner shape and placement using production NVS callbacks.
    kms_desktop_rect_t screens[] = {{0, 960, 1920, 1200}, {1920, 0, 3840, 2160}};
    uint8_t blob[KMS_D_SCREENS_BYTES];
    state->config.stations[0] = (kms_desktop_station_t){.configured = true, .x = 554, .y = 192, .width = 1366, .height = 768};
    psram_config = state->upload.data;
    // Upgrade an existing controller from the legacy placement and screen keys.
    VERIFY(kms_desktop_config_encode(&state->config, state->upload.data));
    memcpy(stored_config, psram_config, sizeof(stored_config)); stored_config_bytes = sizeof(stored_config);
    VERIFY(kms_desktop_screens_encode(screens, 2, blob) && save_screens(NULL, 2, blob) && screen_writes == 1);
    queue_index = 0; kms_desktop_init(true, 777);
    VERIFY(state->config.stations[0].x == 554 && state->config.stations[0].y == 192);
    VERIFY(state->peers[1].count == 2 && !memcmp(state->peers[1].screens, screens, sizeof(screens)));
    VERIFY(!state->peers[1].session && !state->peers[1].geometry && !state->peers[1].ready && !state->peers[1].enabled);
    VERIFY(!state->peers[0].count && !state->peers[2].count && screen_writes == 1);
    VERIFY(stored_layout_bytes == KMS_D_LAYOUT_BYTES && state->reference[1].count == 2);
    // An old extended record has its CRC immediately after the screen snapshots.
    kms_put_u32(stored_layout + KMS_D_LEGACY_LAYOUT_BYTES - 4,
                kms_crc32(stored_layout, KMS_D_LEGACY_LAYOUT_BYTES - 4));
    stored_layout_bytes = KMS_D_LEGACY_LAYOUT_BYTES;
    queue_index = 0; kms_desktop_init(true, 779);
    VERIFY(state->config.gap_tolerance == 8 && state->config.overlap_tolerance == 8);
    VERIFY(state->reference_gap == 0 && state->reference_overlap == 8 && state->reference[1].count == 2);
    state->config.corner = 5;
    state->config.gap_tolerance = UINT32_MAX;
    state->config.overlap_tolerance = 0;
    VERIFY(save_config(NULL, &state->config, state, 0));
    stored_screens[1][20] ^= 1; queue_index = 0; kms_desktop_init(true, 778);
    VERIFY(!state->peers[1].count && state->config.stations[0].x == 554 && state->config.corner == 5);
    VERIFY(state->config.gap_tolerance == UINT32_MAX && state->config.overlap_tolerance == 0);
    VERIFY(state->reference[1].count == 2 && !memcmp(state->reference[1].screens, screens, sizeof(screens)));
    is_master = false; xQueueReset(commands); xQueueReset(outgoing);
    host_session = 22;
    // Power notifications are acknowledged by the receiver without a master reply.
    q = (kms_desktop_message_t){.op = KMS_D_POWER, .id = 91, .session = 22};
    kms_put_u32(q.data, KMS_D_POWER_SLEEP_OR_HIBERNATE); usb_command(&q);
    kms_desktop_link_tick(2, 200, 7, false, true, true);
    s = usb_status(); VERIFY(s.data[45] == 1 && s.data[46] == KMS_D_POWER_SLEEP_OR_HIBERNATE && s.data[47] == 0x81);
    VERIFY(!kms_desktop_jiggler_allowed());
    uint8_t reply[64]; kms_desktop_usb_get(KMS_D_COMMAND_REPORT, reply, sizeof(reply));
    VERIFY(kms_desktop_decode(&s, reply, sizeof(reply)) && s.id == 91 && s.result == KMS_D_OK);
    clock_ms += KMS_D_LEASE_MS; s = usb_status();
    VERIFY(s.data[47] == 0x80 && s.data[46] == KMS_D_POWER_SLEEP_OR_HIBERNATE && !kms_desktop_jiggler_allowed());
    q.session = 21; kms_put_u32(q.data, KMS_D_POWER_AWAKE); usb_command(&q);
    kms_desktop_link_tick(2, 200, 7, false, true, true); VERIFY(!kms_desktop_jiggler_allowed());
    q.session = 22; kms_put_u32(q.data, 99); usb_command(&q);
    kms_desktop_link_tick(2, 200, 7, false, true, true); VERIFY(!kms_desktop_jiggler_allowed());
    kms_put_u32(q.data, KMS_D_POWER_AWAKE); usb_command(&q);
    kms_desktop_link_tick(2, 200, 7, false, true, true); VERIFY(kms_desktop_jiggler_allowed());
    kms_put_u32(q.data, KMS_D_POWER_SHUTDOWN); usb_command(&q);
    kms_desktop_link_tick(2, 200, 7, false, true, true); VERIFY(!kms_desktop_jiggler_allowed());
    kms_desktop_usb_reset(); s = usb_status(); VERIFY(kms_desktop_jiggler_allowed() && !s.data[46]);
    failure = channel_controls();
    return failure ? failure : device_controls();
}
static int channel_controls(void)
{
    // New commands must traverse the production S2 USB-to-radio bridge unchanged.
    host_session = 11; xQueueReset(commands); xQueueReset(outgoing);
    kms_management_frame_t bridge;
    for (unsigned op = KMS_D_CHANNEL_GET; op <= KMS_D_CHANNEL_RESUME; ++op) {
        kms_desktop_message_t request = {.op = (uint8_t)op, .id = op + 600, .session = 11};
        kms_put_u32(request.data, 11); usb_command(&request);
        kms_desktop_link_tick(2, 200, 7, false, true, true);
        VERIFY(kms_desktop_take_outgoing(&bridge));
        kms_desktop_message_t decoded;
        VERIFY(kms_desktop_decode(&decoded, bridge.data, bridge.length) && decoded.op == op && kms_get_u32(decoded.data) == 11);
    }
    is_master = true; xQueueReset(outgoing);
    state->peers[0].session = 11; state->peers[0].boot = 10;
    kms_desktop_message_t q = {.op = KMS_D_CHANNEL_GET, .id = 700, .session = 11}, r;
    kms_management_frame_t f = {.slot = 1, .sender = 10, .target = own_boot, .length = KMS_D_BYTES};
    VERIFY(kms_desktop_encode(&q, f.data)); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length));
    VERIFY(r.result == KMS_D_OK && kms_get_u32(r.data) == 6 && kms_get_u32(r.data + 12) == 13);
    const char *errors[] = {NULL, "BUSY", "STORAGE", "UPGRADE_SLAVES", "INVALID"};
    const unsigned results[] = {KMS_D_OK, KMS_D_BUSY, KMS_D_STORAGE, KMS_D_UNSUPPORTED, KMS_D_INVALID};
    q.op = KMS_D_CHANNEL_SET; kms_put_u32(q.data, 11);
    for (unsigned i = 0; i < 5; ++i) {
        channel_error = errors[i]; f.flags = 0; f.sender = 10; f.target = own_boot;
        kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
        VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) && r.result == results[i]);
    }
    VERIFY(channel_sets == 5);
    q.session = 99; f.flags = 0; f.sender = 10; f.target = own_boot;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) && r.result == KMS_D_STALE && channel_sets == 5);
    q.session = 11; f.flags = 0; f.sender = 99; f.target = own_boot;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) && r.result == KMS_D_STALE && channel_sets == 5);
    q.session = 11; q.op = KMS_D_CHANNEL_RESUME; f.flags = 0; f.sender = 10; f.target = own_boot;
    queued_updates = 1; kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) && r.result == KMS_D_BUSY && !channel_resumes);
    queued_updates = 0; f.flags = 0; f.sender = 10; f.target = own_boot;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) && r.result == KMS_D_OK && channel_resumes == 1);
    kms_desktop_device_handler(channel_recovery_command);
    q.op = KMS_D_CHANNEL_RECOVERY_GET; q.session = 11; q.id = 41;
    f.flags = 0; f.sender = 10; f.target = own_boot;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) &&
           r.result == KMS_D_OK && r.data[2] == 15 && recovery_commands == 1);
    q.op = KMS_D_CHANNEL_RECOVERY_SET; q.session = 99; q.id = 42;
    f.flags = 0; f.sender = 10; f.target = own_boot;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) &&
           r.result == KMS_D_STALE && recovery_commands == 1);
    q.session = 11; q.data[0] = 1; q.data[1] = 3; q.data[2] = 10;
    f.flags = 0; f.sender = 10; f.target = own_boot;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) &&
           r.result == KMS_D_OK && r.data[1] == 3 && r.data[2] == 10 && recovery_commands == 2);
    f.flags = 0; f.sender = 10; f.target = own_boot;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) &&
           r.result == KMS_D_OK && recovery_commands == 2);
    --q.id; f.flags = 0; f.sender = 10; f.target = own_boot;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&f) && kms_desktop_decode(&r, f.data, f.length) &&
           r.result == KMS_D_STALE && recovery_commands == 2);
    memset(device_requests, 0, KMS_SLOTS * sizeof(*device_requests));
    kms_desktop_device_handler(NULL);
    return 0;
}
static int post_update_handover(void)
{
    kms_management_frame_t f;
    kms_desktop_message_t q;
    // An eligible blue receiver can still be protected during a real update.
    // Once confirmation finishes, progress polling must not delay CROSS/READY.
    while (kms_desktop_take_outgoing(&f)) { }
    is_master = true; input_blocked = false;
    kms_desktop_io_t io = state->io; io.routes = &route_storage;
    kms_desktop_master_init(state, &io, 101);
    for (unsigned i = 0; i < 2; ++i) {
        state->config.stations[i].configured = true; state->config.stations[i].x = (int32_t)i * 100;
        state->peers[i] = (kms_desktop_peer_t){.boot = (i + 1) * 10, .session = i + 11,
            .enabled = true, .ready = true, .seen = clock_ms, .geometry = 1, .count = 1};
        state->peers[i].screens[0] = (kms_desktop_rect_t){0, 0, 100, 100};
    }
    cached_firmware.state = KMS_CACHED_CONFIRM;
    kms_desktop_link_tick(1, 42, 77, true, true, true); VERIFY(!state->idle);
    kms_management_frame_t progress = {.op = KMS_M_CACHE_PROGRESS};
    for (unsigned i = 0; i < 20; ++i) kms_remote_upload_reply(&direct_firmware, &progress, KMS_M_OK, clock_ms);
    cached_firmware.state = KMS_CACHED_UPDATED;
    kms_desktop_link_tick(1, 42, 77, true, true, true); VERIFY(state->idle);
    q = (kms_desktop_message_t){.op = KMS_D_CROSS, .id = 500, .session = 11};
    kms_put_u32(q.data, 77); kms_put_u32(q.data + 4, KMS_D_RIGHT);
    kms_put_u32(q.data + 8, 99); kms_put_u32(q.data + 12, 50); kms_put_u32(q.data + 16, 1);
    f = (kms_management_frame_t){.op = KMS_M_DESKTOP, .slot = 1, .sender = 10, .target = own_boot, .length = KMS_D_BYTES};
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(state->handoff.active && state->handoff.target == 2);
    VERIFY(kms_desktop_take_outgoing(&f)); kms_desktop_message_t reply;
    VERIFY(kms_desktop_decode(&reply, f.data, f.length) && reply.result == KMS_D_OK);
    kms_desktop_link_tick(1, 42, 77, true, true, true);
    VERIFY(kms_desktop_take_outgoing(&f) && f.slot == 2 && f.target == 20);
    VERIFY(kms_desktop_decode(&reply, f.data, f.length) && reply.op == KMS_D_PREPARE);
    q = (kms_desktop_message_t){.op = KMS_D_READY, .id = 501, .session = 12};
    kms_put_u32(q.data, state->handoff.id); f.slot = 2; f.sender = 20; f.target = own_boot; f.flags = 0;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(selected_station == 2 && !state->handoff.active);
    const unsigned terminal[] = {KMS_CACHED_SKIPPED, KMS_CACHED_FAILED};
    for (unsigned i = 0; i < 2; ++i) {
        cached_firmware.state = terminal[i];
        kms_desktop_link_tick(1, 42, 77, true, true, true); VERIFY(state->idle);
    }
    queued_updates = 1; kms_desktop_link_tick(1, 42, 77, true, true, true); VERIFY(!state->idle);
    queued_updates = 0; cache_upload_busy = true;
    kms_desktop_link_tick(1, 42, 77, true, true, true); VERIFY(!state->idle);
    cache_upload_busy = false;
    kms_desktop_link_tick(1, 42, 77, true, true, true); VERIFY(state->idle);
    return 0;
}
__declspec(dllexport) int kms_desktop_runtime_test_count(void) { return (int)runtime_checks; }

static unsigned device_calls;
static bool device_control(unsigned source, uint32_t boot, kms_desktop_message_t *q)
{
    if (source != 1 || boot != 10) return false;
    ++device_calls;
    if (q->op == KMS_D_USB_SNAPSHOT) return false;
    kms_put_u32(q->data, 7);
    return true;
}
static int device_controls(void)
{
    is_master = true;
    state->peers[0].session = 11; state->peers[0].boot = 10;
    kms_desktop_device_handler(device_control);
    device_calls = 0;
    kms_desktop_message_t q = {.op = KMS_D_DEVICE_CONTROL, .id = 600, .session = 11}, reply;
    kms_management_frame_t f = {.op = KMS_M_DESKTOP, .slot = 1, .sender = 10, .target = own_boot, .length = KMS_D_BYTES}, out;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&out) && kms_desktop_decode(&reply, out.data, out.length) &&
        reply.result == KMS_D_OK && kms_get_u32(reply.data) == 7 && device_calls == 1);
    kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&out) && kms_desktop_decode(&reply, out.data, out.length) &&
        kms_get_u32(reply.data) == 7 && device_calls == 1);
    kms_put_u32(q.data, 2); kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&out) && kms_desktop_decode(&reply, out.data, out.length) &&
        reply.result == KMS_D_INVALID && device_calls == 1);
    q.id = 601; q.op = KMS_D_USB_SNAPSHOT;
    kms_desktop_encode(&q, f.data); kms_desktop_receive(&f); kms_desktop_receive(&f);
    VERIFY(!kms_desktop_take_outgoing(&out) && device_calls == 2);
    kms_desktop_device_reply(1, 20, &q); VERIFY(!kms_desktop_take_outgoing(&out));
    kms_desktop_device_reply(1, 10, &q);
    VERIFY(kms_desktop_take_outgoing(&out) && kms_desktop_decode(&reply, out.data, out.length) && reply.id == q.id);
    q.id = 600; q.op = KMS_D_DEVICE_CONTROL; kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&out) && kms_desktop_decode(&reply, out.data, out.length) &&
        reply.result == KMS_D_STALE && device_calls == 2);
    q.id = 602; q.session = 12; kms_desktop_encode(&q, f.data); kms_desktop_receive(&f);
    VERIFY(kms_desktop_take_outgoing(&out) && kms_desktop_decode(&reply, out.data, out.length) &&
        reply.result == KMS_D_STALE && device_calls == 2);
    return 0;
}
