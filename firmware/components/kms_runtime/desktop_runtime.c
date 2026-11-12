#include "kms_desktop_runtime.h"
#include "kms_desktop_routes.h"
#include "kms_runtime.h"
#include "kms_remote.h"
#include "kms_radio_channel.h"
#include "kms_test_mode.h"
#include "kms_memory.h"
#include "kms_stream_radio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include <string.h>

static bool is_master, reset_pending, input_blocked;
static uint32_t own_boot, master_boot, own_slot, host_session, prepared_at;
static QueueHandle_t commands, outgoing;
static QueueHandle_t clipboard_incoming;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static kms_desktop_master_t *state;
static kms_desktop_routes_t *routing;
static kms_desktop_message_t response, status, prepare;
static kms_desktop_preferences_fn preferences;
static kms_desktop_device_fn device_handler;
typedef struct {
    kms_desktop_message_t request, reply;
    uint32_t boot, at;
    bool valid, pending;
} device_request_t;
static device_request_t *device_requests;
static uint8_t host_power;
static uint32_t power_seen;
static kms_clipboard_diagnostics_t clipboard_stats;
typedef struct {
    uint32_t id, sender, target;
    uint8_t slot, flags, data[KMS_D_BYTES];
} desktop_frame_t;

static void clipboard_count(uint32_t *counter)
{ portENTER_CRITICAL(&lock); ++*counter; portEXIT_CRITICAL(&lock); }
void kms_clipboard_diagnostics(kms_clipboard_diagnostics_t *out)
{
    portENTER_CRITICAL(&lock); *out = clipboard_stats; portEXIT_CRITICAL(&lock);
    out->commands = 0;
    out->incoming = clipboard_incoming ? uxQueueMessagesWaiting(clipboard_incoming) : 0;
    out->outgoing = kms_stream_pending();
}

static bool send_message(void *ctx, unsigned slot, const kms_desktop_message_t *m)
{
    (void)ctx;
    if (!slot || slot > KMS_SLOTS || (is_master && !state)) return false;
    desktop_frame_t f = {.slot = (uint8_t)slot, .id = m->id,
        .sender = own_boot, .target = is_master ? state->peers[slot - 1].boot : master_boot,
        .flags = is_master ? KMS_M_REPLY : 0};
    return kms_desktop_encode(m, f.data) && outgoing && xQueueSend(outgoing, &f, 0) == pdTRUE;
}
static bool save_config(void *ctx, const kms_desktop_config_t *config, const kms_desktop_master_t *m, uint8_t mask)
{
    (void)ctx; nvs_handle_t h;
    esp_err_t e = nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return false;
    // A pending desktop upload can live in PSRAM. Keep flash write data on
    // the application's internal stack.
    uint8_t staging[KMS_D_LAYOUT_BYTES];
    if (!kms_desktop_layout_encode(m, config, mask, staging)) { nvs_close(h); return false; }
    e = nvs_set_blob(h, "desk_layout", staging, sizeof(staging));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); return e == ESP_OK;
}
static bool select_station(void *ctx, unsigned slot)
{ (void)ctx; return kms_link_select(NULL, slot); }
static bool station_available(void *ctx, unsigned slot)
{ (void)ctx; return kms_link_desktop_available(slot); }
static void screens_key(char *key, unsigned slot)
{
    memcpy(key, "desktop_s0", 11); key[9] = (char)('0' + slot);
}
static bool save_screens(void *ctx, unsigned slot, const uint8_t *data)
{
    (void)ctx; nvs_handle_t h; char key[11]; screens_key(key, slot);
    esp_err_t e = nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return false;
    e = nvs_set_blob(h, key, data, KMS_D_SCREENS_BYTES);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); return e == ESP_OK;
}
void kms_desktop_preferences_handler(kms_desktop_preferences_fn fn) { preferences = fn; }
void kms_desktop_device_handler(kms_desktop_device_fn fn)
{
    if (fn && !device_requests) device_requests = kms_memory_calloc(KMS_SLOTS, sizeof(*device_requests));
    device_handler = fn;
}
void kms_desktop_device_reply(unsigned source, uint32_t boot, const kms_desktop_message_t *m)
{
    if (!is_master || !state || !device_requests || !source || source > KMS_SLOTS) return;
    kms_desktop_peer_t *p = &state->peers[source - 1];
    unsigned i = source - 1;
    if (!device_requests[i].valid || device_requests[i].boot != boot ||
        device_requests[i].request.session != m->session || device_requests[i].request.id != m->id ||
        device_requests[i].request.op != m->op || p->session != m->session || p->boot != boot) return;
    device_requests[i].reply = *m; device_requests[i].pending = false; device_requests[i].at = kms_now();
    send_message(NULL, source, m);
}
void kms_desktop_init(bool master, uint32_t boot)
{
    is_master = master; own_boot = boot;
    if (master && !state) state = kms_memory_calloc(1, sizeof(*state));
    if (master && !state) return;
    if (master && !routing) routing = kms_memory_calloc(1, sizeof(*routing));
    commands = kms_memory_queue_create(8, sizeof(kms_desktop_message_t));
    outgoing = kms_memory_queue_create(8, sizeof(desktop_frame_t));
    clipboard_incoming = kms_memory_queue_create(4, KMS_C_BYTES);
    if (master) {
        const kms_desktop_io_t io = {.send = send_message, .save = save_config, .save_screens = save_screens,
            .select = select_station, .available = station_available, .routes = routing};
        kms_desktop_master_init(state, &io, boot);
        nvs_handle_t h;
        bool migrate = false;
        if (nvs_open(KMS_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            uint8_t data[KMS_D_LAYOUT_BYTES]; size_t n = sizeof(data);
            if (nvs_get_blob(h, "desk_layout", data, &n) != ESP_OK || !kms_desktop_layout_decode(state, data, n)) {
                n = KMS_D_CONFIG_BYTES;
                migrate = nvs_get_blob(h, "desktop", data, &n) == ESP_OK &&
                    kms_desktop_config_decode(&state->config, data, n);
            }
            for (unsigned i = 0; i < KMS_SLOTS; ++i) {
                char key[11]; uint8_t screens[KMS_D_SCREENS_BYTES]; n = sizeof(screens); screens_key(key, i + 1);
                if (nvs_get_blob(h, key, screens, &n) == ESP_OK)
                    (void)kms_desktop_screens_decode(&state->peers[i], screens, n);
            }
            nvs_close(h);
        }
        if (migrate) {
            kms_desktop_capture_reference(state, UINT8_MAX);
            (void)save_config(NULL, &state->config, state, 0);
        }
    }
}
void kms_desktop_usb_reset(void)
{
    if (commands) xQueueReset(commands);
    if (clipboard_incoming) xQueueReset(clipboard_incoming);
    portENTER_CRITICAL(&lock); reset_pending = true; host_session = 0;
    host_power = KMS_D_POWER_UNKNOWN;
    memset(&response, 0, sizeof(response)); memset(&prepare, 0, sizeof(prepare)); portEXIT_CRITICAL(&lock);
}
void kms_desktop_usb_set(const uint8_t *data, size_t n)
{
    kms_desktop_message_t q;
    // Windows HIDAPI pads short feature reports to the collection's maximum length.
    if (!data || (n != KMS_D_BYTES && n != KMS_C_BYTES)) return;
    for (size_t i = KMS_D_BYTES; i < n; ++i) if (data[i]) return;
    if (commands && kms_desktop_decode(&q, data, KMS_D_BYTES) && q.id && q.session && !q.result &&
        xQueueSend(commands, &q, 0) == pdTRUE)
        kms_app_wake();
}
size_t kms_desktop_usb_get(unsigned report, uint8_t *out, size_t capacity)
{
    if (capacity < KMS_D_BYTES) return 0;
    portENTER_CRITICAL(&lock);
    kms_desktop_message_t m = report == KMS_D_STATUS_REPORT ? status : response;
    if (report == KMS_D_STATUS_REPORT) {
        m.session = host_session;
        m.data[46] = host_power;
        m.data[47] = 0x80u | (host_power != KMS_D_POWER_UNKNOWN &&
            !kms_time_elapsed(kms_now(), power_seen, KMS_D_LEASE_MS) ? 1u : 0u);
        if (prepare.id && !kms_time_elapsed(kms_now(), prepared_at, KMS_D_HANDOFF_MS)) {
            kms_put_u32(m.data + 24, prepare.id); memcpy(m.data + 28, prepare.data, 12);
        } else memset(m.data + 24, 0, 16);
    }
    portEXIT_CRITICAL(&lock);
    if (report == KMS_D_STATUS_REPORT) {
        // Status.id is unused by the request/reply mailbox. Advertise a tagged,
        // CRC-protected snapshot without growing the report or adding a USB read.
        unsigned incoming = clipboard_incoming ? uxQueueMessagesWaiting(clipboard_incoming) : 0;
        m.id = kms_stream_local_status(m.data[0], incoming);
    }
    if (!m.op) m.op = KMS_D_STATUS;
    return kms_desktop_encode(&m, out) ? KMS_D_BYTES : 0;
}
void kms_desktop_local_reply(const kms_desktop_message_t *reply)
{
    portENTER_CRITICAL(&lock);
    if (!is_master && reply->session == host_session) response = *reply;
    portEXIT_CRITICAL(&lock);
}
void kms_desktop_input(const kms_sample_t *sample)
{
    if (is_master) return;
    bool held = sample->state.modifiers || sample->state.buttons || sample->state.system;
    for (unsigned i = 0; i < KMS_KEY_BYTES; ++i) held |= sample->state.keys[i] != 0;
    for (unsigned i = 0; i < KMS_CONSUMERS; ++i) held |= sample->state.consumer[i] != 0;
    portENTER_CRITICAL(&lock); status.data[2] = held;
    if (sample->x || sample->y) {
        kms_put_u32(status.data + 12, kms_get_u32(status.data + 12) + 1);
        kms_put_u32(status.data + 16, (uint32_t)sample->x); kms_put_u32(status.data + 20, (uint32_t)sample->y);
    }
    portEXIT_CRITICAL(&lock);
}
void kms_desktop_motion(int32_t x, int32_t y) { if (is_master && state) kms_desktop_master_motion(state, x, y); }
void kms_desktop_block(bool blocked) { input_blocked = blocked; }
bool kms_desktop_jiggler_allowed(void)
{
    portENTER_CRITICAL(&lock);
    bool allowed = host_power == KMS_D_POWER_UNKNOWN || host_power == KMS_D_POWER_AWAKE;
    portEXIT_CRITICAL(&lock); return allowed;
}
bool kms_desktop_take_outgoing(kms_management_frame_t *f)
{
    desktop_frame_t queued;
    if (!outgoing || xQueueReceive(outgoing, &queued, 0) != pdTRUE) return false;
    *f = (kms_management_frame_t){.op = KMS_M_DESKTOP, .length = KMS_D_BYTES,
        .slot = queued.slot, .flags = queued.flags, .id = queued.id,
        .sender = queued.sender, .target = queued.target};
    memcpy(f->data, queued.data, sizeof(queued.data));
    return true;
}
void kms_clipboard_usb_set(const uint8_t *data, size_t n)
{
    kms_clipboard_packet_t p;
    if (!kms_clipboard_decode(&p, data, n)) return;
    clipboard_count(&clipboard_stats.usb_received);
    portENTER_CRITICAL(&lock);
    bool accepted = p.source == own_slot && p.source_session == host_session && master_boot && status.data[3];
    kms_management_frame_t f = {.op = KMS_M_CLIPBOARD, .slot = (uint8_t)own_slot,
        .id = p.sequence, .sender = own_boot, .target = master_boot, .length = KMS_C_BYTES};
    portEXIT_CRITICAL(&lock);
    if (!accepted || !kms_clipboard_encode(&p, f.data) || !kms_stream_enqueue(&f))
        clipboard_count(&clipboard_stats.queue_drops);
    else kms_app_wake();
}
size_t kms_clipboard_usb_get(uint8_t *out, size_t capacity)
{
    if (capacity < KMS_C_BYTES) return 0;
    memset(out, 0, KMS_C_BYTES);
    if (clipboard_incoming) (void)xQueueReceive(clipboard_incoming, out, 0);
    return KMS_C_BYTES;
}
void kms_clipboard_receive(const kms_management_frame_t *f)
{
    kms_clipboard_packet_t p;
    if (!kms_clipboard_decode(&p, f->data, f->length)) return;
    clipboard_count(&clipboard_stats.radio_received);
    if (is_master) {
        if (!state || f->target != own_boot || f->flags ||
            !kms_clipboard_authorized(state, &p, f->slot, f->sender, kms_now())) {
            clipboard_count(&clipboard_stats.unauthorized); return;
        }
        kms_management_frame_t relay = *f;
        relay.slot = p.target; relay.sender = own_boot; relay.target = state->peers[p.target - 1].boot;
        relay.flags = KMS_M_REPLY;
        if (kms_stream_enqueue_profile(&relay, kms_stream_radio_source_profile(f->slot)))
            clipboard_count(&clipboard_stats.relayed);
        else clipboard_count(&clipboard_stats.queue_drops);
    } else if (f->target == own_boot && f->sender == master_boot && f->flags == KMS_M_REPLY) {
        portENTER_CRITICAL(&lock); bool accepted = p.target == own_slot && p.target_session == host_session;
        portEXIT_CRITICAL(&lock);
        if (!accepted) clipboard_count(&clipboard_stats.unauthorized);
        else if (clipboard_incoming && xQueueSend(clipboard_incoming, f->data, 0) == pdTRUE)
            clipboard_count(&clipboard_stats.delivered);
        else clipboard_count(&clipboard_stats.queue_drops);
    } else clipboard_count(&clipboard_stats.unauthorized);
}
void kms_desktop_receive(const kms_management_frame_t *f)
{
    kms_desktop_message_t m;
    if (!kms_desktop_decode(&m, f->data, f->length)) return;
    if (is_master) {
        if (!state || !f->slot || f->slot > KMS_SLOTS) return;
        if ((f->target != own_boot && !(m.op == KMS_D_HELLO && !f->target)) || f->flags) return;
        if (m.op >= KMS_D_DEVICE_CONTROL && m.op <= KMS_D_CHANNEL_RECOVERY_SET) {
            unsigned i = f->slot - 1;
            kms_desktop_peer_t *p = &state->peers[i];
            if (!m.id || m.result || !m.session || p->session != m.session || p->boot != f->sender) {
                m.result = KMS_D_STALE; send_message(NULL, f->slot, &m); return;
            }
            if (!device_handler || !device_requests) {
                m.result = KMS_D_UNSUPPORTED; send_message(NULL, f->slot, &m); return;
            }
            if (device_requests[i].valid && device_requests[i].boot == f->sender &&
                device_requests[i].request.session == m.session) {
                if (device_requests[i].request.id == m.id) {
                    if (device_requests[i].request.op != m.op ||
                        memcmp(device_requests[i].request.data, m.data, sizeof(m.data))) {
                        m.result = KMS_D_INVALID; send_message(NULL, f->slot, &m);
                    } else if (!device_requests[i].pending) send_message(NULL, f->slot, &device_requests[i].reply);
                    return;
                }
                if ((uint32_t)(m.id - device_requests[i].request.id) >= 0x80000000u) {
                    m.result = KMS_D_STALE; send_message(NULL, f->slot, &m); return;
                }
                if (device_requests[i].pending && !kms_time_elapsed(kms_now(), device_requests[i].at, 10000)) {
                    m.result = KMS_D_BUSY; send_message(NULL, f->slot, &m); return;
                }
            }
            device_requests[i].request = m; device_requests[i].boot = f->sender;
            device_requests[i].at = kms_now(); device_requests[i].valid = device_requests[i].pending = true;
            if (device_handler(f->slot, f->sender, &m)) kms_desktop_device_reply(f->slot, f->sender, &m);
        } else if (m.op >= KMS_D_CHANNEL_GET && m.op <= KMS_D_CHANNEL_RESUME) {
            kms_desktop_peer_t *p = &state->peers[f->slot - 1];
            if (!m.session || p->session != m.session || p->boot != f->sender) m.result = KMS_D_STALE;
            else if (m.op == KMS_D_CHANNEL_SET) {
                const char *error = kms_radio_channel_set(kms_get_u32(m.data));
                if (error) {
                    if (!strcmp(error, "BUSY")) m.result = KMS_D_BUSY;
                    else if (!strcmp(error, "STORAGE")) m.result = KMS_D_STORAGE;
                    else if (!strcmp(error, "UPGRADE_SLAVES")) m.result = KMS_D_UNSUPPORTED;
                    else m.result = KMS_D_INVALID;
                }
            } else if (m.op == KMS_D_CHANNEL_RESUME) {
                if (kms_radio_channel_changing() || kms_link_pairing() || kms_remote_transfer_busy() || kms_test_mode_active()) m.result = KMS_D_BUSY;
                else {
                    kms_link_resume();
                    if (!kms_link_selected()) m.result = KMS_D_OFFLINE;
                }
            }
            kms_radio_channel_status_t s;
            kms_radio_channel_snapshot(&s);
            memset(m.data, 0, sizeof(m.data));
            kms_put_u32(m.data, s.current); kms_put_u32(m.data + 4, s.pending);
            kms_put_u32(m.data + 8, s.first); kms_put_u32(m.data + 12, s.count);
            kms_put_u32(m.data + 16, s.paired); kms_put_u32(m.data + 20, s.supported);
            send_message(NULL, f->slot, &m);
        } else if (m.op == KMS_D_RECOVERY_STATUS) {
            kms_desktop_peer_t *p = &state->peers[f->slot - 1];
            if (p->session != m.session || p->boot != f->sender) m.result = KMS_D_STALE;
            else if (!kms_link_recovery_status(kms_get_u32(m.data), m.data)) m.result = KMS_D_INVALID;
            send_message(NULL, f->slot, &m);
        } else if ((m.op == KMS_D_PREFERENCES_GET || m.op == KMS_D_PREFERENCES_SET) && preferences) {
            kms_desktop_peer_t *p = &state->peers[f->slot - 1];
            uint32_t revision = kms_get_u32(m.data); uint8_t data[KMS_PREFERENCES_BYTES]; memcpy(data, m.data + 4, sizeof(data));
            if (p->session != m.session || p->boot != f->sender) m.result = KMS_D_STALE;
            else if (!preferences(m.op == KMS_D_PREFERENCES_SET, data, &revision)) m.result = KMS_D_CONFLICT;
            memcpy(m.data + 4, data, sizeof(data)); kms_put_u32(m.data, revision); send_message(NULL, f->slot, &m);
        } else kms_desktop_master_request(state, f->slot, f->sender, &m, kms_now());
    } else if (f->target == own_boot && (f->flags & KMS_M_REPLY)) {
        portENTER_CRITICAL(&lock);
        if (m.session == host_session) {
            master_boot = f->sender;
            if (m.op == KMS_D_PREPARE) { prepare = m; prepared_at = kms_now(); }
            else response = m;
        }
        portEXIT_CRITICAL(&lock);
    }
}
void kms_desktop_link_tick(unsigned slot, uint32_t controller_boot, uint32_t generation, bool active, bool usb_ready, bool idle)
{
    if (is_master) {
        if (state) kms_desktop_master_tick(state, slot, generation,
            idle && !input_blocked && !kms_link_pairing() && !kms_remote_transfer_busy() && !kms_radio_channel_changing(), kms_now());
        return;
    }
    portENTER_CRITICAL(&lock);
    own_slot = slot;
    status.op = KMS_D_STATUS; status.data[0] = (uint8_t)slot; status.data[1] = active; status.data[3] = usb_ready;
    status.data[44] = 1; // Clipboard channel capability, absent on older receivers.
    status.data[45] = 1; // Local USB journal and handshake, independent of the radio.
    kms_put_u32(status.data + 4, own_boot); kms_put_u32(status.data + 8, master_boot);
    kms_put_u32(status.data + 40, generation);
    // An inactive HID guard can retain an earlier controller boot. Desktop
    // discovery establishes its own master epoch through HELLO replies.
    (void)controller_boot;
    bool reset = reset_pending; reset_pending = false;
    portEXIT_CRITICAL(&lock);
    if (reset && outgoing) xQueueReset(outgoing);
    if (reset) kms_stream_drop_clipboard();
    kms_desktop_message_t q;
    for (unsigned i = 0; i < 4 && commands && xQueueReceive(commands, &q, 0) == pdTRUE; ++i) {
        portENTER_CRITICAL(&lock);
        bool new_session = (q.op == KMS_D_HELLO || q.op == KMS_D_LOCAL_HELLO) && host_session != q.session;
        if (new_session) {
            host_session = q.session; memset(&response, 0, sizeof(response)); memset(&prepare, 0, sizeof(prepare)); master_boot = 0;
            host_power = KMS_D_POWER_UNKNOWN;
        }
        bool accepted = host_session == q.session && usb_ready;
        if (accepted && q.op == KMS_D_POWER) {
            uint32_t value = kms_get_u32(q.data);
            response = q;
            if (value > KMS_D_POWER_SHUTDOWN) { response.result = KMS_D_INVALID; accepted = false; }
            else { host_power = (uint8_t)value; power_seen = kms_now(); }
            // Acknowledge locally: sleeping must not wait for a radio round trip.
        }
        if (q.op == KMS_D_READY && kms_get_u32(q.data) == prepare.id) prepare.id = 0;
        portEXIT_CRITICAL(&lock);
        if (new_session && outgoing) xQueueReset(outgoing);
        if (new_session) {
            if (clipboard_incoming) xQueueReset(clipboard_incoming);
            kms_stream_drop_clipboard();
        }
        if (accepted && q.op == KMS_D_LOCAL_HELLO) {
            memset(q.data, 0, sizeof(q.data)); kms_put_u32(q.data, own_boot); kms_desktop_local_reply(&q);
        } else if (accepted && q.op >= KMS_D_LOG_BEGIN && q.op <= KMS_D_LOG_END) {
            if (!kms_remote_local_diagnostics(&q)) { q.result = KMS_D_BUSY; kms_desktop_local_reply(&q); }
        } else if (accepted && own_slot) (void)send_message(NULL, own_slot, &q);
    }
}
