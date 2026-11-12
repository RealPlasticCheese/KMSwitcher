#include "../../firmware/components/kms_runtime/channel_recovery.c"

#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
enum quality { GOOD, BAD_MAC, SLOW_MAC, SLOW_RTT, PROCESSING, BUSY_USB, TIMEOUT, NO_CALLBACK, REJECT };
static unsigned checks, writes, pauses, finishes, probes, probe_violations, switches, commits, disowns;
static unsigned current, saved, requested, country_first, country_count;
static uint32_t clock_ms, switch_at, challenge, last_tx, last_contact[KMS_SLOTS], boots[KMS_SLOTS];
static bool trial, pending, held, guarded, driver_blocked, idle, observing, retune_fail, save_fail, commit_fail;
static bool refuse_switch;
static bool have_store, restore_route;
static uint8_t storage[STORE_BYTES], current_peers, candidate_peers, supported;
static enum quality normal[KMS_SLOTS], candidate[KMS_SLOTS];
static unsigned probe_count[KMS_SLOTS];

uint32_t kms_now(void) { return clock_ms; }
uint32_t esp_random(void) { return ++challenge; }
void kms_fake_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *h)
{ (void)space; (void)mode; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{
    (void)h; (void)key;
    if (!have_store) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(data, storage, sizeof(storage)); *size = sizeof(storage);
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    (void)h; (void)key;
    if (save_fail || size != sizeof(storage)) return ESP_FAIL;
    memcpy(storage, data, size); have_store = true; ++writes;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
bool kms_radio_channel_trial_active(void) { return trial; }
bool kms_radio_channel_switch_pending(void) { return pending; }
bool kms_radio_channel_recovering(void) { return trial || held; }
unsigned kms_radio_channel_get(void) { return current; }
bool kms_radio_channel_trial_available(void) { return !guarded && !held && !trial; }
void kms_radio_channel_snapshot(kms_radio_channel_status_t *out)
{ *out = (kms_radio_channel_status_t){current, pending ? requested : current, country_first, country_count, 0, 0}; }
bool kms_radio_channel_trial_switch(unsigned ch)
{
    if (refuse_switch || pending || (!trial && !kms_radio_channel_trial_available()) ||
        ch < country_first || ch >= country_first + country_count) return false;
    // The durable budget must be reserved before putting a notice on air.
    if (!have_store || !storage[3]) ++probe_violations;
    ++switches; trial = pending = true; requested = ch; switch_at = clock_ms + KMS_RADIO_SWITCH_MS;
    return true;
}
void kms_radio_channel_trial_cancel_switch(void) { pending = false; }
bool kms_radio_channel_trial_commit(void)
{
    ++commits;
    if (commit_fail) return false;
    saved = current; return true;
}
void kms_radio_channel_trial_end(void) { trial = pending = false; }
void kms_link_channel_recovery_pause(void) { ++pauses; }
void kms_link_channel_recovery_finish(bool restore) { ++finishes; restore_route = restore; }
void kms_link_channel_recovery_disown(void) { ++disowns; }
bool kms_radio_channel_probe(unsigned slot, uint32_t boot, uint32_t token)
{
    if (!idle || pending || (probes && clock_ms - last_tx < PROBE_PERIOD_MS) || boots[slot - 1] != boot || !token)
        ++probe_violations;
    last_tx = clock_ms; ++probes; ++probe_count[slot - 1];
    return (current == 6 ? normal : candidate)[slot - 1] != REJECT;
}
static unsigned configure(unsigned flags, unsigned minutes)
{
    kms_desktop_message_t m = {.op = KMS_D_CHANNEL_RECOVERY_SET, .data = {SCHEMA, (uint8_t)flags, (uint8_t)minutes}};
    kms_channel_recovery_command(&m);
    return m.result;
}
static void fixture(void)
{
    clock_ms = 1000; current = saved = 6; country_first = 1; country_count = 13;
    writes = pauses = finishes = probes = probe_violations = switches = commits = disowns = 0;
    trial = pending = held = guarded = driver_blocked = retune_fail = save_fail = commit_fail = false;
    refuse_switch = false;
    idle = observing = true; have_store = restore_route = false;
    current_peers = candidate_peers = 3; supported = 127;
    memset(normal, 0, sizeof(normal)); memset(candidate, 0, sizeof(candidate));
    memset(last_contact, 0, sizeof(last_contact)); memset(probe_count, 0, sizeof(probe_count));
    for (unsigned i = 0; i < KMS_SLOTS; ++i) boots[i] = 100 + i;
    kms_channel_recovery_init();
}
static void complete_probe(uint32_t token, unsigned slot, enum quality quality)
{
    uint32_t start = clock_ms + 2;
    if (quality == NO_CALLBACK || quality == REJECT) return;
    uint32_t elapsed = quality == SLOW_MAC ? 95 : quality == BAD_MAC ? 110 : 4;
    kms_channel_recovery_mac(token, start, elapsed, quality == BAD_MAC);
    if (quality == BAD_MAC || quality == TIMEOUT) return;
    uint16_t turnaround = quality == PROCESSING ? 70 : 3;
    uint32_t arrival = start + elapsed + turnaround + (quality == SLOW_RTT ? 80 : 3);
    kms_radio_message_t reply = {.type = KMS_RADIO_PROBE_REPLY, .slot = (uint8_t)slot, .nonce = token,
        .boot = boots[slot - 1], .flags = quality == BUSY_USB ? KMS_RADIO_PROBE_BUSY : 0, .delay_ms = turnaround};
    uint32_t at = clock_ms;
    clock_ms = arrival;
    kms_channel_recovery_reply(&reply, arrival);
    clock_ms = at;
}
static void step(void)
{
    if (pending && (int32_t)(clock_ms - switch_at) >= 0) {
        pending = false;
        if (!retune_fail) current = requested;
    }
    uint8_t online = current == 6 ? current_peers : candidate_peers;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        if (online & slot_bit(i + 1)) last_contact[i] = clock_ms;
        kms_channel_recovery_peer(i + 1, last_contact[i] ? boots[i] : 0, last_contact[i],
                                  (supported & slot_bit(i + 1)) != 0);
    }
    unsigned before = probes;
    kms_channel_recovery_tick(clock_ms, 1, observing, idle, driver_blocked);
    if (probes != before && recovery.probe.token) {
        unsigned slot = recovery.probe.slot;
        enum quality quality = online & slot_bit(slot) ? (current == 6 ? normal : candidate)[slot - 1] : BAD_MAC;
        complete_probe(recovery.probe.token, slot, quality);
    }
}
static void drive(unsigned duration)
{
    for (unsigned elapsed = 0; elapsed < duration; elapsed += 250) { clock_ms += 250; step(); }
}
static void degrade_both(void) { normal[0] = normal[1] = BAD_MAC; }
static int settings_and_samples(void)
{
    fixture();
    CHECK(!kms_channel_recovery_monitoring() && recovery.settings == SINGLE_PEER && recovery.cooldown == DEFAULT_COOLDOWN);
    degrade_both(); drive(10000); CHECK(!probes && !switches);
    CHECK(!kms_channel_recovery_single_reachable(clock_ms));
    CHECK(configure(4, 15) == KMS_D_INVALID && configure(ENABLED, 4) == KMS_D_INVALID);
    CHECK(configure(ENABLED, 61) == KMS_D_INVALID && !writes);
    kms_desktop_message_t m = {.op = KMS_D_CHANNEL_RECOVERY_SET, .data = {SCHEMA, ENABLED, 15, 1}};
    kms_channel_recovery_command(&m); CHECK(m.result == KMS_D_INVALID && !writes);
    m = (kms_desktop_message_t){.op = KMS_D_CHANNEL_RECOVERY_SET, .data = {2, ENABLED, 15}};
    kms_channel_recovery_command(&m); CHECK(m.result == KMS_D_INVALID && !writes);
    CHECK(configure(ENABLED | SINGLE_PEER, 5) == KMS_D_OK && writes == 1);
    CHECK(configure(ENABLED | SINGLE_PEER, 5) == KMS_D_OK && writes == 1);
    m = (kms_desktop_message_t){.op = KMS_D_CHANNEL_RECOVERY_GET};
    kms_channel_recovery_command(&m);
    CHECK(m.data[0] == 1 && m.data[1] == 3 && m.data[2] == 5 && m.data[11] == MAX_ATTEMPTS);
    idle = false; drive(10000); CHECK(!probes && !switches);
    idle = true; observing = false; drive(10000); CHECK(!probes && !switches);
    observing = true; supported = 1; drive(10000); CHECK(!probes && !switches);
    supported = 127; held = true; drive(10000); CHECK(!probes && !switches);
    held = false; driver_blocked = true; drive(10000); CHECK(!probes && !switches);
    driver_blocked = false;
    normal[0] = PROCESSING; normal[1] = BUSY_USB; drive(20000);
    CHECK(probes > 5 && !switches && !recovery.peers[0].samples.bad && recovery.peers[0].samples.processing);
    normal[0] = TIMEOUT; normal[1] = NO_CALLBACK; drive(30000); CHECK(!switches);
    normal[0] = normal[1] = REJECT; drive(10000); CHECK(!switches && !probe_violations);
    save_fail = true;
    CHECK(configure(SINGLE_PEER, 5) == KMS_D_STORAGE && !kms_channel_recovery_monitoring() && disowns == 1);
    save_fail = false; CHECK(configure(SINGLE_PEER, 5) == KMS_D_OK && storage[1] == SINGLE_PEER);
    storage[0] = 99; kms_channel_recovery_init();
    CHECK(!kms_channel_recovery_monitoring() && configure(ENABLED, 15) == KMS_D_STORAGE);
    return 0;
}
static int multi_and_rollback(void)
{
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    normal[1] = BAD_MAC; drive(75000);
    CHECK(probe_count[0] && probe_count[1] && !switches);
    normal[0] = SLOW_MAC; drive(40000);
    CHECK(pauses == 1 && switches == 1);
    CHECK(commits == 1 && recovery.last == KEPT);
    CHECK(recovery.reason == MULTIPLE_LINKS && recovery.affected == 3 && saved == 13 && current == 13);
    CHECK(restore_route && recovery.attempts == 1 && !probe_violations);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    candidate_peers = 1; current_peers = 7; degrade_both(); drive(18000);
    CHECK(trial && current == 13 && saved == 6 && recovery.expected == 7 && !commits);
    candidate_peers = 7; drive(10000);
    CHECK(recovery.last == KEPT && saved == 13 && recovery.expected == 7 && !probe_violations);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    candidate_peers = 1; degrade_both(); drive(70000);
    CHECK(recovery.last == REVERTED && current == 6 && saved == 6 && switches == 2 &&
          !commits && restore_route && recovery.expected == 3);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); candidate[0] = candidate[1] = BAD_MAC; drive(70000);
    CHECK(recovery.last == REVERTED && switches == 2 && !commits && saved == 6);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); commit_fail = true; drive(55000);
    CHECK(recovery.last == STORAGE && commits == 1 && saved == 6 && current == 6 && restore_route);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); retune_fail = true; drive(55000);
    CHECK(recovery.last == FAILED && !commits && switches == 1 && current == 6 && restore_route);
    return 0;
}
static int sample_epochs(void)
{
    fixture(); CHECK(configure(ENABLED | SINGLE_PEER, 15) == KMS_D_OK);
    drive(10000);
    CHECK(recovery.peers[0].samples.samples >= 2 && recovery.peers[0].samples.contact_gap == 250);
    kms_channel_recovery_input_retry(1);
    kms_channel_recovery_input_retry(7);
    CHECK(recovery.peers[0].samples.input_retries == 1 && !recovery.peers[6].samples.input_retries);
    ++boots[0]; drive(250);
    CHECK(!recovery.peers[1].samples.samples && !recovery.peers[0].samples.input_retries);
    recovery.probe = (probe_t){.slot = 1, .boot = boots[0], .token = 321, .scheduled_at = clock_ms};
    kms_radio_message_t reply = {.slot = 1, .nonce = 320, .boot = boots[0], .type = KMS_RADIO_PROBE_REPLY};
    kms_channel_recovery_reply(&reply, clock_ms); CHECK(!recovery.probe.replied);
    reply.nonce = 321; reply.slot = 2; kms_channel_recovery_reply(&reply, clock_ms); CHECK(!recovery.probe.replied);
    reply.slot = 1; --reply.boot; kms_channel_recovery_reply(&reply, clock_ms); CHECK(!recovery.probe.replied);
    ++reply.boot; kms_channel_recovery_reply(&reply, clock_ms + KMS_RADIO_PROBE_TIMEOUT_MS);
    CHECK(!recovery.probe.replied);
    uint32_t scheduled = clock_ms;
    kms_channel_recovery_mac(321, scheduled + PROCESSING_SLOW_MS, 5, false);
    clock_ms += 40;
    kms_channel_recovery_reply(&reply, clock_ms);
    CHECK(recovery.probe.replied && !recovery.probe.local_delay);
    reply.flags = KMS_RADIO_PROBE_BUSY; kms_channel_recovery_reply(&reply, clock_ms);
    CHECK(!recovery.probe.local_delay); // A duplicate cannot replace the matched response.
    take_sample(clock_ms);
    CHECK(recovery.peers[0].samples.processing == 1 && !recovery.peers[0].samples.samples);
    return 0;
}
static int single_and_budget(void)
{
    fixture(); CHECK(configure(ENABLED | SINGLE_PEER, 5) == KMS_D_OK);
    normal[0] = BAD_MAC; drive(20000); CHECK(!switches && !kms_channel_recovery_single_reachable(clock_ms));
    current_peers = candidate_peers = 1; drive(60000);
    CHECK(!switches && recovery.observed == 1 && kms_channel_recovery_single_reachable(clock_ms));
    drive(18000); CHECK(recovery.last == KEPT && recovery.reason == SINGLE_LINK && recovery.expected == 1);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    current_peers = candidate_peers = 1; normal[0] = BAD_MAC; drive(120000); CHECK(!switches);
    CHECK(configure(ENABLED | SINGLE_PEER, 5) == KMS_D_OK);
    normal[0] = SLOW_RTT; drive(65000); CHECK(!switches); // RTT alone cannot qualify a single link.
    normal[0] = BAD_MAC; drive(32000); CHECK(!switches);
    normal[0] = GOOD; drive(32000); CHECK(!switches && !recovery.solo_windows);
    normal[0] = BAD_MAC; drive(32000); CHECK(!switches);
    drive(42000); CHECK(recovery.last == KEPT && switches == 1);
    fixture(); CHECK(configure(ENABLED, 5) == KMS_D_OK);
    guarded = true; degrade_both(); drive(45000); CHECK(!switches && writes == 1);
    guarded = false; save_fail = true; drive(20000);
    CHECK(!switches && recovery.last == STORAGE && recovery.cooling);
    save_fail = false; drive(310000); CHECK(switches == 1 && recovery.attempts == 1);
    // Power loss retains the spent budget and the avoided channels.
    uint16_t avoided = recovery.tried;
    kms_channel_recovery_init();
    CHECK(recovery.attempts == 1 && recovery.cooling && recovery.tried == avoided);
    CHECK(configure(0, 5) == KMS_D_OK && configure(ENABLED, 5) == KMS_D_OK && recovery.attempts == 1);
    current = saved = 6; drive(295000); CHECK(switches == 1);
    drive(30000); CHECK(switches == 2 && recovery.attempts == MAX_ATTEMPTS && recovery.candidate != 13);
    current = saved = 6; drive(600000); CHECK(switches == 2);
    drive(BUDGET_MS); CHECK(switches == 3 && recovery.attempts == 1);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    country_first = 6; country_count = 1; degrade_both(); drive(45000);
    CHECK(!switches && !recovery.attempts);
    country_first = 1; country_count = 11; refuse_switch = true;
    drive(25000);
    CHECK(!switches && recovery.attempts == 1 && recovery.cooling && recovery.last == FAILED);
    return 0;
}
static int interruptions(void)
{
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); while (recovery.phase == OBSERVE && clock_ms < 25000) drive(250);
    CHECK(recovery.phase == ANNOUNCE && saved == 6);
    CHECK(configure(0, 15) == KMS_D_OK && !pending && disowns == 1);
    drive(RECONNECT_MS + 1000); CHECK(recovery.last == CANCELLED && saved == 6 && !kms_channel_recovery_monitoring());
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); while (recovery.phase != VERIFY && clock_ms < 25000) drive(250);
    CHECK(recovery.phase == VERIFY);
    trial = false; saved = current; drive(250);
    CHECK(recovery.last == MANUAL && !restore_route && switches == 1);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); while (recovery.phase == OBSERVE && clock_ms < 25000) drive(250);
    CHECK(recovery.phase == ANNOUNCE);
    kms_channel_recovery_driver_stall(); drive(RECONNECT_MS + 1000);
    CHECK(recovery.last == DRIVER && current == 6 && !commits && switches == 1);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); while (recovery.phase != VERIFY && clock_ms < 25000) drive(250);
    CHECK(recovery.phase == VERIFY);
    driver_blocked = true; kms_channel_recovery_driver_stall(); drive(250);
    CHECK(recovery.last == FAILED && !restore_route && saved == 6 && !pending);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); while (recovery.phase != VERIFY && clock_ms < 25000) drive(250);
    CHECK(recovery.phase == VERIFY);
    observing = false; refuse_switch = true;
    drive(VERIFY_MS + 250);
    CHECK(recovery.last == FAILED && !restore_route && saved == 6 && switches == 1);
    fixture(); CHECK(configure(ENABLED, 15) == KMS_D_OK);
    degrade_both(); candidate[0] = candidate[1] = BAD_MAC;
    while (recovery.phase != ROLLBACK && clock_ms < 70000) drive(250);
    CHECK(recovery.phase == ROLLBACK && switches == 2);
    kms_channel_recovery_driver_stall(); drive(250);
    CHECK(recovery.phase == ROLLBACK && recovery.rollback_retried && switches == 3);
    kms_channel_recovery_driver_stall(); drive(250);
    CHECK(recovery.last == FAILED && !restore_route && saved == 6 && !pending);
    return 0;
}
__declspec(dllexport) int kms_run_channel_recovery_unit(void)
{
    checks = 0;
    int failure = settings_and_samples();
    if (!failure) failure = multi_and_rollback();
    if (!failure) failure = sample_epochs();
    if (!failure) failure = single_and_budget();
    return failure ? failure : interruptions();
}
__declspec(dllexport) int kms_run_channel_recovery_stress(void)
{
    checks = 0; fixture(); CHECK(configure(ENABLED | SINGLE_PEER, 5) == KMS_D_OK);
    // Interleave bursts of HID and management traffic with narrow send windows.
    // The selected peer stays healthy while other radios fail, leave and return.
    for (unsigned i = 0; i < 2400; ++i) {
        idle = i % 9 == 8 || i % 13 == 12;
        current_peers = i < 600 || i >= 1800 ? 127 : i < 1200 ? 3 : 1;
        normal[1] = i % 200 < 150 ? BAD_MAC : GOOD;
        normal[2] = i % 41 ? PROCESSING : GOOD;
        observing = i % 300 < 280;
        drive(250);
        CHECK(!switches && !pauses && !probe_violations);
    }
    for (unsigned i = 0; i < KMS_SLOTS; ++i) CHECK(probe_count[i] > 5);
    CHECK(probes < 600 && current == 6);
    // Under the same foreground contention, two impaired links can still qualify
    // once background transmit opportunities accumulate. A late peer must verify.
    fixture(); CHECK(configure(ENABLED, 5) == KMS_D_OK);
    degrade_both(); candidate_peers = 1;
    for (unsigned i = 0; i < 240; ++i) {
        idle = i % 4 == 3;
        if (current != 6 && clock_ms - recovery.stage_at >= 9000u) candidate_peers = 3;
        drive(250);
        CHECK(!probe_violations && switches <= 1);
    }
    CHECK(recovery.last == KEPT && switches == 1 && pauses == 1 && commits == 1 && restore_route && saved == 13);
    return 0;
}
__declspec(dllexport) unsigned kms_channel_recovery_checks(void) { return checks; }
