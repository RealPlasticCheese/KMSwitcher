// Exercise the production channel coordinator with fake Wi-Fi, NVS and clock.
// The deterministic MAC substitutes for PSA only; real HMAC uses ESP-IDF/PSA.
#include "../../firmware/components/kms_runtime/radio_channel.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, pauses, releases, wifi_channel, sends, failures;
static uint32_t clock_ms, random_value;
static bool transfer_busy, test_active, ota_busy, nvs_fail, restart_pending, wifi_fail;
static uint8_t nvs_channel, nvs_capable[KMS_SLOTS][6], captured[KMS_RADIO_BYTES], imported[16];
static const uint8_t test_mac[6] = {1,2,3,4,5,6}, other_mac[6] = {1,2,3,4,5,7};
static const uint8_t test_key[16] = {8,9,10};
static int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
uint32_t kms_now(void) { return clock_ms; }
uint32_t esp_random(void) { return ++random_value; }
bool kms_link_pairing(void) { return false; }
bool kms_remote_transfer_busy(void) { return transfer_busy; }
bool kms_test_mode_active(void) { return test_active; }
bool kms_ota_busy(void) { return ota_busy; }
bool kms_remote_restart_pending(void) { return restart_pending; }
bool kms_link_receiver_restart_in_progress(void) { return restart_pending; }
void kms_link_pause(void) { ++pauses; }
static void release(void) { ++releases; }
static bool send(const uint8_t *mac, const uint8_t *data, size_t bytes)
{ (void)mac; if (bytes != sizeof(captured)) ++failures; memcpy(captured, data, bytes); ++sends; return true; }
esp_err_t esp_wifi_get_country(wifi_country_t *c) { *c = (wifi_country_t){1,13}; return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t ch, int second)
{ (void)second; if (wifi_fail) return ESP_FAIL; wifi_channel = ch; return ESP_OK; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "fake"; }
void kms_fake_log(const char *tag_name, const char *format, ...) { (void)tag_name; (void)format; }
void kms_fake_check(int e) { if (e != ESP_OK) ++failures; }
int printf(const char *format, ...) { (void)format; return 0; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h)
{ (void)name; (void)mode; *h = 1; return nvs_fail ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *name, void *data, size_t *bytes)
{
    (void)h;
    if (!strcmp(name, "radio_channel")) {
        if (!nvs_channel) return ESP_ERR_NVS_NOT_FOUND;
        *(uint8_t *)data = nvs_channel; *bytes = 1;
    } else memcpy(data, nvs_capable, *bytes);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *name, const void *data, size_t bytes)
{
    (void)h; (void)bytes;
    if (!strcmp(name, "radio_channel")) nvs_channel = *(const uint8_t *)data;
    else memcpy(nvs_capable, data, sizeof(nvs_capable));
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void psa_set_key_type(psa_key_attributes_t *a, int t) { (void)a; (void)t; }
void psa_set_key_usage_flags(psa_key_attributes_t *a, int t) { (void)a; (void)t; }
void psa_set_key_algorithm(psa_key_attributes_t *a, int t) { (void)a; (void)t; }
void psa_destroy_key(psa_key_id_t id) { (void)id; }
void psa_reset_key_attributes(psa_key_attributes_t *a) { (void)a; }
int psa_import_key(const psa_key_attributes_t *a, const uint8_t *key, size_t bytes, psa_key_id_t *id)
{ (void)a; memcpy(imported, key, bytes); *id = 1; return PSA_SUCCESS; }
int psa_mac_compute(psa_key_id_t id, int alg, const uint8_t *data, size_t bytes, uint8_t *out, size_t size, size_t *used)
{
    (void)id; (void)alg; (void)size;
    memset(out, 0, 32);
    for (size_t i = 0; i < bytes; ++i) out[i % 16] ^= data[i] ^ imported[i % 16];
    *used = 32; return PSA_SUCCESS;
}
static void encode(kms_radio_message_t m, uint8_t *wire)
{
    if (!kms_radio_encode(&m, wire) || !tag(test_key, wire, wire + KMS_RADIO_HEADER)) ++failures;
}
static void start(bool is_master, uint32_t boot)
{
    kms_radio_channel_init(is_master, boot, send, release);
    kms_radio_channel_peer(1, test_mac, test_key);
}
static void receive_wire(const uint8_t *mac, const uint8_t *wire, size_t bytes)
{ (void)kms_radio_channel_receive(mac, wire, bytes, clock_ms, NULL); }
static int authenticated_probes_and_trials(void)
{
    nvs_channel = 6; memset(nvs_capable, 0, sizeof(nvs_capable)); clock_ms = 100;
    start(true, 123);
    CHECK(!kms_radio_channel_trial_available() && !kms_radio_channel_trial_switch(11));
    kms_radio_message_t m = {.type = KMS_RADIO_QUERY, .slot = 1, .channel = 6, .boot = 456, .nonce = 99};
    uint8_t wire[KMS_RADIO_BYTES];
    encode(m, wire); receive_wire(test_mac, wire, sizeof(wire));
    restart_pending = true;
    CHECK(!kms_radio_channel_trial_available() && !strcmp(kms_radio_channel_set(11), "BUSY"));
    CHECK(!kms_radio_channel_set(6) && !kms_radio_channel_changing());
    restart_pending = false;
    CHECK(kms_radio_channel_trial_available() && !kms_radio_channel_trial_switch(14));
    CHECK(kms_radio_channel_probe(1, 456, 100) && kms_radio_decode(&m, captured, sizeof(captured)));
    CHECK(m.type == KMS_RADIO_PROBE && m.channel == 6 && m.boot == 456 && m.nonce == 100);
    m.type = KMS_RADIO_PROBE_REPLY; encode(m, wire);
    kms_radio_message_t reply;
    CHECK(!kms_radio_channel_receive(other_mac, wire, sizeof(wire), clock_ms, &reply));
    CHECK(kms_radio_channel_receive(test_mac, wire, sizeof(wire), clock_ms, &reply) && reply.nonce == 100);
    CHECK(kms_radio_channel_trial_switch(11) && nvs_channel == 6 && kms_radio_channel_recovering());
    CHECK(!kms_radio_channel_probe(1, 456, 101) && !kms_radio_channel_trial_commit());
    clock_ms += KMS_RADIO_SWITCH_MS;
    kms_radio_channel_tick(false, false, true); CHECK(kms_radio_channel_get() == 6);
    kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_channel_get() == 11 && !kms_radio_channel_switch_pending() && kms_radio_channel_changing() && nvs_channel == 6);
    CHECK(kms_radio_channel_trial_commit() && nvs_channel == 11);
    kms_radio_channel_trial_end();
    CHECK(!kms_radio_channel_changing() && kms_radio_channel_recovering() && !kms_radio_channel_trial_available());
    clock_ms += 12000; kms_radio_channel_tick(false, false, false);
    CHECK(!kms_radio_channel_recovering() && kms_radio_channel_trial_switch(6));
    // Manual override owns the chosen channel even during the announcement.
    CHECK(!kms_radio_channel_set(1) && !kms_radio_channel_trial_active() && nvs_channel == 1);
    clock_ms += KMS_RADIO_SWITCH_MS; kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_channel_get() == 1 && kms_radio_channel_recovering());
    clock_ms += 12000; kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_channel_trial_switch(11)); wifi_fail = true;
    clock_ms += KMS_RADIO_SWITCH_MS; kms_radio_channel_tick(false, false, false); wifi_fail = false;
    CHECK(kms_radio_channel_get() == 1 && nvs_channel == 1 && !kms_radio_channel_switch_pending());
    kms_radio_channel_trial_end();
    // The S2 answers independently of USB and Companion, from the radio task path.
    nvs_channel = 6; clock_ms += 100; start(false, 456);
    m = (kms_radio_message_t){.type = KMS_RADIO_PROBE, .slot = 1, .channel = 6, .boot = 455, .nonce = 88};
    unsigned before = sends;
    encode(m, wire); receive_wire(test_mac, wire, sizeof(wire));
    kms_radio_channel_probe_reply_tick(false); CHECK(sends == before);
    m.boot = 456; encode(m, wire); wire[20] ^= 1; receive_wire(test_mac, wire, sizeof(wire));
    kms_radio_channel_probe_reply_tick(false); CHECK(sends == before);
    encode(m, wire); receive_wire(test_mac, wire, sizeof(wire));
    kms_radio_channel_processing_fault(clock_ms); clock_ms += 8;
    kms_radio_channel_probe_reply_tick(false);
    CHECK(sends == before + 1 && kms_radio_decode(&reply, captured, sizeof(captured)) &&
          reply.type == KMS_RADIO_PROBE_REPLY && reply.nonce == 88 && reply.delay_ms == 8 && reply.flags == KMS_RADIO_PROBE_BUSY);
    ++before;
    for (unsigned guard = 0; guard < 4; ++guard) {
        receive_wire(test_mac, wire, sizeof(wire));
        ota_busy = guard == 0; restart_pending = guard == 1;
        if (guard == 2) clock_ms += KMS_RADIO_PROBE_TIMEOUT_MS;
        kms_radio_channel_probe_reply_tick(guard == 3);
        ota_busy = restart_pending = false;
        kms_radio_channel_probe_reply_tick(false);
        CHECK(sends == before);
    }
    return 0;
}
__declspec(dllexport) int kms_run_radio_channel_tests(void)
{
    checks = failures = pauses = releases = sends = 0; clock_ms = 100;
    start(true, 123);
    CHECK(kms_radio_channel_get() == 6);
    kms_radio_channel_status_t status;
    kms_radio_channel_snapshot(&status);
    CHECK(status.current == 6 && status.pending == 6 && status.first == 1 && status.count == 13 && status.paired == 1 && !status.supported);
    CHECK(!strcmp(kms_radio_channel_set(11), "UPGRADE_SLAVES"));
    CHECK(!kms_radio_channel_set(6) && nvs_channel == 6 && pauses == 0);
    uint8_t wire[KMS_RADIO_BYTES];
    kms_radio_message_t m = {.type = KMS_RADIO_QUERY, .slot = 1, .channel = 6, .boot = 456, .nonce = 99};
    encode(m, wire); wire[20] ^= 1;
    receive_wire(test_mac, wire, sizeof(wire));
    CHECK(!strcmp(kms_radio_channel_set(11), "UPGRADE_SLAVES"));
    encode(m, wire); receive_wire(other_mac, wire, sizeof(wire));
    CHECK(!strcmp(kms_radio_channel_set(11), "UPGRADE_SLAVES"));
    receive_wire(test_mac, wire, sizeof(wire));
    transfer_busy = true; CHECK(!strcmp(kms_radio_channel_set(11), "BUSY")); transfer_busy = false;
    test_active = true; CHECK(!strcmp(kms_radio_channel_set(11), "BUSY")); test_active = false;
    nvs_fail = true; CHECK(!strcmp(kms_radio_channel_set(11), "STORAGE") && pauses == 0); nvs_fail = false;
    CHECK(!kms_radio_channel_set(11) && pauses == 1 && kms_radio_channel_changing());
    kms_radio_channel_snapshot(&status);
    CHECK(status.current == 6 && status.pending == 11 && status.paired == status.supported);
    kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_decode(&m, captured, sizeof(captured)) && m.type == KMS_RADIO_REPLY && m.channel == 11 && m.delay_ms == 1800);
    clock_ms += 1900; kms_radio_channel_tick(false, false, true);
    CHECK(kms_radio_channel_get() == 6); // pending transmitter prevents retune
    kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_channel_get() == 11 && wifi_channel == 11 && !kms_radio_channel_changing());
    start(true, 124); CHECK(kms_radio_channel_get() == 11);
    CHECK(!kms_radio_channel_set(1)); // previously confirmed but offline slave
    // Fresh receiver: only a reply to its outstanding challenge can lock discovery.
    nvs_channel = 0; memset(nvs_capable, 0, sizeof(nvs_capable)); clock_ms = 10000;
    start(false, 456); kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_decode(&m, captured, sizeof(captured)) && m.type == KMS_RADIO_QUERY);
    m.type = KMS_RADIO_REPLY; m.channel = 11; m.delay_ms = 100;
    encode(m, wire); wire[20] ^= 1; receive_wire(test_mac, wire, sizeof(wire));
    CHECK(!kms_radio_channel_changing() && releases == 0);
    encode(m, wire); receive_wire(other_mac, wire, sizeof(wire));
    CHECK(!kms_radio_channel_changing());
    ++m.nonce; encode(m, wire); receive_wire(test_mac, wire, sizeof(wire));
    CHECK(!kms_radio_channel_changing());
    --m.nonce; encode(m, wire); receive_wire(test_mac, wire, sizeof(wire));
    CHECK(kms_radio_channel_changing() && releases == 1);
    clock_ms += 200; kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_channel_get() == 11);
    clock_ms += 1000; kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_decode(&m, captured, sizeof(captured)));
    m.type = KMS_RADIO_REPLY; m.channel = 11; m.delay_ms = 0;
    encode(m, wire); receive_wire(test_mac, wire, sizeof(wire));
    CHECK(nvs_channel == 11);
    // Power-off/missed notification recovery retains pairing/key and searches.
    clock_ms += 5000; ota_busy = true; kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_channel_get() == 11); ota_busy = false;
    for (unsigned i = 0; i < 13 && kms_radio_channel_get() != 1; ++i) {
        clock_ms += 400; kms_radio_channel_tick(false, false, false);
    }
    CHECK(kms_radio_channel_get() == 1 && releases > 1);
    CHECK(kms_radio_decode(&m, captured, sizeof(captured)) && m.channel == 1);
    m.type = KMS_RADIO_REPLY; encode(m, wire); receive_wire(test_mac, wire, sizeof(wire));
    CHECK(nvs_channel == 1);
    m.channel = 13; encode(m, wire); receive_wire(test_mac, wire, sizeof(wire));
    CHECK(!kms_radio_channel_changing()); // replay after challenge consumption
    start(false, 457); CHECK(kms_radio_channel_get() == 1);
    // A valid legacy management heartbeat keeps upgraded receivers on channel.
    for (unsigned i = 0; i < 6; ++i) {
        clock_ms += 1000; kms_radio_channel_contact(); kms_radio_channel_tick(false, false, false);
        CHECK(kms_radio_channel_get() == 1);
    }
    // Failed physical pairing restores the saved channel; a candidate freezes hopping.
    clock_ms += 500; kms_radio_channel_tick(true, false, false);
    CHECK(kms_radio_channel_get() == 2);
    clock_ms += 500; kms_radio_channel_tick(true, true, false);
    CHECK(kms_radio_channel_get() == 2);
    kms_radio_channel_tick(false, false, false);
    CHECK(kms_radio_channel_get() == 1);
    CHECK(failures == 0);
    return authenticated_probes_and_trials();
}
__declspec(dllexport) int kms_radio_channel_test_count(void) { return (int)checks; }
