#include "kms_channel_recovery.h"
#include "kms_radio_channel.h"
#include "kms_runtime.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include <string.h>

enum {
    OBSERVE_MS = 30000, CONTACT_MS = 6000, PROBE_PERIOD_MS = 1000, PEER_PERIOD_MS = 3000,
    SLOW_BACKOFF_MS = 2200, MAC_SLOW_MS = 45, RTT_SLOW_MS = 55, PROCESSING_SLOW_MS = 30,
    MIN_SAMPLES = 3, MIN_SPAN_MS = 5000, SOLO_WINDOWS = 2,
    SWITCH_LIMIT_MS = KMS_RADIO_SWITCH_MS + 2000, VERIFY_MS = 32000, RECONNECT_MS = 20000,
    MAX_ATTEMPTS = 2, BUDGET_MS = 3600000, DEFAULT_COOLDOWN = 15, MIN_COOLDOWN = 5, MAX_COOLDOWN = 60,
    ENABLED = 1, SINGLE_PEER = 2, SCHEMA = 1, STORE_BYTES = 8
};
enum phase { OBSERVE, ANNOUNCE, VERIFY, ROLLBACK, RECONNECT };
enum result { NO_RESULT, KEPT, REVERTED, CANCELLED, FAILED, MANUAL, DRIVER, STORAGE };
enum reason { NO_REASON, MULTIPLE_LINKS, SINGLE_LINK };
typedef struct {
    uint32_t first, rtt_total, contact_gap, input_retries;
    uint8_t samples, replies, slow, failed, timeouts, mac_bad, bad, processing;
} samples_t;
typedef struct {
    uint32_t boot, seen, last_probe;
    bool supported, paced;
    samples_t samples;
} peer_t;
typedef struct {
    uint32_t token, boot, scheduled_at, started, mac_ms, received_at;
    uint16_t turnaround_ms;
    uint8_t slot;
    bool completed, failed, replied, local_delay;
} probe_t;
typedef struct { uint32_t token, started, elapsed; bool failed; } completion_t;
static const char *TAG = "channel-recovery";
static const char *STORE = "channel_recover";
static portMUX_TYPE completion_lock = portMUX_INITIALIZER_UNLOCKED;
static completion_t completion;
static struct {
    peer_t peers[KMS_SLOTS];
    samples_t baseline[KMS_SLOTS];
    probe_t probe;
    enum phase phase;
    enum result last, rollback_result, interrupt;
    enum reason reason;
    uint32_t window_at, budget_at, cooldown_at, stage_at, last_probe;
    uint16_t tried, probe_interval;
    uint8_t settings, cooldown, attempts, next_slot, observed, expected, affected, old, candidate, solo_windows;
    bool storage_ok, cooling, paced, rollback_retried, settings_dirty;
} recovery;

static bool valid_settings(unsigned flags, unsigned cooldown)
{
    return flags <= (ENABLED | SINGLE_PEER) && cooldown >= MIN_COOLDOWN && cooldown <= MAX_COOLDOWN;
}
static bool save(unsigned settings, unsigned cooldown, unsigned attempts, unsigned tried)
{
    uint8_t bytes[STORE_BYTES] = {SCHEMA, (uint8_t)settings, (uint8_t)cooldown, (uint8_t)attempts,
                                 (uint8_t)tried, (uint8_t)(tried >> 8)};
    nvs_handle_t h;
    if (nvs_open(KMS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, STORE, bytes, sizeof(bytes));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
void kms_channel_recovery_init(void)
{
    memset(&recovery, 0, sizeof(recovery));
    completion = (completion_t){0};
    recovery.settings = SINGLE_PEER;
    recovery.cooldown = DEFAULT_COOLDOWN;
    recovery.budget_at = recovery.window_at = kms_now();
    nvs_handle_t h;
    esp_err_t err = nvs_open(KMS_NVS_NAMESPACE, NVS_READONLY, &h);
    uint8_t bytes[STORE_BYTES];
    size_t size = sizeof(bytes);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, STORE, bytes, &size);
        nvs_close(h);
    }
    recovery.storage_ok = err == ESP_ERR_NVS_NOT_FOUND;
    if (err == ESP_OK && size == sizeof(bytes) && bytes[0] == SCHEMA &&
        valid_settings(bytes[1], bytes[2]) && bytes[3] <= MAX_ATTEMPTS &&
        !(bytes[5] & 0xe0) && !bytes[6] && !bytes[7] &&
        ((bytes[4] || bytes[5]) == (bytes[3] != 0))) {
        recovery.settings = bytes[1];
        recovery.cooldown = bytes[2];
        recovery.attempts = bytes[3];
        recovery.tried = (uint16_t)(bytes[4] | (bytes[5] << 8));
        recovery.storage_ok = true;
        // Without wall-clock time, a power cycle starts a fresh hold for spent attempts.
        recovery.cooling = recovery.attempts != 0;
        recovery.cooldown_at = recovery.budget_at;
    }
    if (!recovery.storage_ok) ESP_LOGW(TAG, "KMSCHANNELRECOVERY disabled: invalid or unavailable storage");
}
bool kms_channel_recovery_monitoring(void)
{
    return (recovery.settings & ENABLED) || recovery.phase != OBSERVE;
}
static uint8_t slot_bit(unsigned slot) { return (uint8_t)(1u << (slot - 1)); }
static void reset_samples(uint32_t now)
{
    for (unsigned i = 0; i < KMS_SLOTS; ++i) recovery.peers[i].samples = (samples_t){0};
    recovery.probe.token = 0;
    recovery.window_at = now;
}
void kms_channel_recovery_peer(unsigned slot, uint32_t boot, uint32_t seen, bool supported)
{
    peer_t *p = &recovery.peers[slot - 1];
    if (p->boot != boot) {
        *p = (peer_t){.boot = boot};
        if (recovery.probe.slot == slot) recovery.probe.token = 0;
        if (recovery.phase == OBSERVE && (recovery.observed & slot_bit(slot))) {
            recovery.solo_windows = 0;
            reset_samples(kms_now());
        }
    }
    if (boot && p->seen && p->seen != seen && seen - p->seen > p->samples.contact_gap)
        p->samples.contact_gap = seen - p->seen;
    p->seen = seen;
    p->supported = supported;
}
void kms_channel_recovery_input_retry(unsigned slot)
{
    if (recovery.observed & slot_bit(slot)) ++recovery.peers[slot - 1].samples.input_retries;
}
static uint8_t reachable(uint32_t now, bool supported)
{
    uint8_t mask = 0;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        const peer_t *p = &recovery.peers[i];
        if (p->boot && now - p->seen < CONTACT_MS && (!supported || p->supported)) mask |= slot_bit(i + 1);
    }
    return mask;
}
static bool only_one(uint8_t mask) { return mask && !(mask & (mask - 1)); }
bool kms_channel_recovery_single_reachable(uint32_t now) { return only_one(reachable(now, false)); }
void kms_channel_recovery_mac(uint32_t token, uint32_t started, uint32_t elapsed, bool failed)
{
    portENTER_CRITICAL(&completion_lock);
    completion = (completion_t){token, started, elapsed, failed};
    portEXIT_CRITICAL(&completion_lock);
}
void kms_channel_recovery_reply(const kms_radio_message_t *m, uint32_t received_at)
{
    probe_t *p = &recovery.probe;
    if (!p->token || m->nonce != p->token || m->slot != p->slot || m->boot != p->boot || p->replied) return;
    if (received_at - p->scheduled_at >= KMS_RADIO_PROBE_TIMEOUT_MS) return;
    p->replied = true;
    p->local_delay = m->flags || m->delay_ms >= PROCESSING_SLOW_MS ||
                     kms_now() - received_at >= PROCESSING_SLOW_MS;
    p->received_at = received_at;
    p->turnaround_ms = m->delay_ms;
}
static void take_sample(uint32_t now)
{
    probe_t *p = &recovery.probe;
    if (!p->token) return;
    portENTER_CRITICAL(&completion_lock);
    completion_t done = completion;
    completion.token = 0;
    portEXIT_CRITICAL(&completion_lock);
    if (done.token == p->token) {
        p->completed = true;
        p->started = done.started;
        p->mac_ms = done.elapsed;
        p->failed = done.failed;
        if (p->failed || p->mac_ms >= MAC_SLOW_MS) recovery.probe_interval = SLOW_BACKOFF_MS;
    }
    if ((!p->completed || !p->replied) && now - p->scheduled_at < KMS_RADIO_PROBE_TIMEOUT_MS) return;
    p->token = 0;
    if (!p->completed) return;
    samples_t *s = &recovery.peers[p->slot - 1].samples;
    uint32_t rtt = p->received_at - p->started;
    if (p->local_delay || p->started - p->scheduled_at >= PROCESSING_SLOW_MS ||
        (p->replied && (rtt >= KMS_RADIO_PROBE_TIMEOUT_MS || rtt < p->turnaround_ms))) {
        ++s->processing;
        return;
    }
    if (!s->samples) s->first = p->started;
    ++s->samples;
    bool rtt_slow = p->replied && rtt - p->turnaround_ms >= RTT_SLOW_MS;
    s->failed += p->failed;
    s->slow += p->mac_ms >= MAC_SLOW_MS || rtt_slow;
    s->timeouts += !p->replied;
    bool mac_bad = p->failed || p->mac_ms >= MAC_SLOW_MS;
    s->mac_bad += mac_bad;
    s->bad += mac_bad || rtt_slow;
    if (p->replied) { ++s->replies; s->rtt_total += rtt; }
}
static void send_probe(uint32_t now, uint8_t mask)
{
    if (!mask || recovery.probe.token || (recovery.paced && now - recovery.last_probe < recovery.probe_interval)) return;
    for (unsigned n = 0; n < KMS_SLOTS; ++n) {
        unsigned index = recovery.next_slot;
        recovery.next_slot = (uint8_t)((index + 1) % KMS_SLOTS);
        peer_t *p = &recovery.peers[index];
        if (!(mask & slot_bit(index + 1)) || (p->paced && now - p->last_probe < PEER_PERIOD_MS)) continue;
        uint32_t token;
        do { token = esp_random(); } while (!token);
        if (kms_radio_channel_probe(index + 1, p->boot, token)) {
            recovery.probe = (probe_t){.token = token, .boot = p->boot, .scheduled_at = now, .slot = (uint8_t)(index + 1)};
            p->last_probe = now;
            p->paced = true;
        }
        // Pace even an SDK rejection. A skipped send supplies no radio evidence.
        recovery.paced = true;
        recovery.last_probe = now;
        recovery.probe_interval = PROBE_PERIOD_MS;
        return;
    }
}
static uint8_t degraded(uint32_t now, uint8_t mask, bool solo)
{
    uint8_t bad = 0;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        const samples_t *s = &recovery.peers[i].samples;
        unsigned count = solo ? s->mac_bad : s->bad;
        if ((mask & slot_bit(i + 1)) && s->samples >= MIN_SAMPLES && now - s->first >= MIN_SPAN_MS &&
            count >= 2 && count * 2 >= s->samples && s->mac_bad) bad |= slot_bit(i + 1);
    }
    return bad;
}
static unsigned choose_channel(unsigned current, unsigned first, unsigned count)
{
    unsigned best = 0, separation = 0;
    for (unsigned channel = first; channel < first + count; ++channel) {
        unsigned distance = channel > current ? channel - current : current - channel;
        if (distance > separation && !(recovery.tried & (1u << (channel - 1)))) {
            best = channel;
            separation = distance;
        }
    }
    return best;
}
static void log_samples(const char *phase_name)
{
    for (unsigned i = 0; i < KMS_SLOTS; ++i) if (recovery.expected & slot_bit(i + 1)) {
        const samples_t *s = &recovery.peers[i].samples;
        ESP_LOGI(TAG, "KMSCHANNELRECOVERY %s slot=%u probes=%u replies=%u slow=%u failed=%u timeouts=%u"
            " processing=%u rtt-ms=%lu input-retries=%lu contact-gap-ms=%lu",
            phase_name, i + 1, s->samples, s->replies, s->slow, s->failed, s->timeouts, s->processing,
            (unsigned long)(s->replies ? s->rtt_total / s->replies : 0),
            (unsigned long)s->input_retries, (unsigned long)s->contact_gap);
    }
}
static void finish(enum result outcome, bool restore, uint32_t now)
{
    if (kms_radio_channel_trial_active()) kms_radio_channel_trial_end();
    ESP_LOGI(TAG, "KMSCHANNELRECOVERY result=%u old=%u candidate=%u current=%u expected=0x%x attempts=%u",
        outcome, recovery.old, recovery.candidate, kms_radio_channel_get(), recovery.expected, recovery.attempts);
    recovery.last = outcome;
    recovery.phase = OBSERVE;
    recovery.interrupt = NO_RESULT;
    recovery.cooling = true;
    recovery.cooldown_at = now;
    recovery.observed = recovery.solo_windows = 0;
    reset_samples(now);
    kms_link_channel_recovery_finish(restore);
}
static void begin_trial(uint32_t now, uint8_t expected, uint8_t affected, bool solo)
{
    if (!recovery.storage_ok || recovery.attempts >= MAX_ATTEMPTS || recovery.cooling ||
        !kms_radio_channel_trial_available()) return;
    kms_radio_channel_status_t status;
    kms_radio_channel_snapshot(&status);
    unsigned channel = choose_channel(status.current, status.first, status.count);
    if (!channel) return;
    unsigned tried = recovery.tried | (1u << (channel - 1)) | (1u << (status.current - 1));
    if (!save(recovery.settings, recovery.cooldown, recovery.attempts + 1, tried)) {
        ESP_LOGW(TAG, "KMSCHANNELRECOVERY attempt blocked: cannot save trial budget");
        recovery.cooling = true;
        recovery.cooldown_at = now;
        recovery.last = STORAGE;
        return;
    }
    recovery.budget_at = now;
    ++recovery.attempts;
    recovery.tried = (uint16_t)tried;
    if (!kms_radio_channel_trial_switch(channel)) {
        recovery.cooling = true;
        recovery.cooldown_at = now;
        recovery.last = FAILED;
        ESP_LOGW(TAG, "KMSCHANNELRECOVERY could not announce channel=%u attempts=%u", channel, recovery.attempts);
        return;
    }
    recovery.old = (uint8_t)status.current;
    recovery.candidate = (uint8_t)channel;
    recovery.expected = expected;
    recovery.affected = affected;
    recovery.reason = solo ? SINGLE_LINK : MULTIPLE_LINKS;
    recovery.phase = ANNOUNCE;
    recovery.last = NO_RESULT;
    recovery.stage_at = now;
    recovery.rollback_retried = false;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) recovery.baseline[i] = recovery.peers[i].samples;
    ESP_LOGI(TAG, "KMSCHANNELRECOVERY start reason=%u old=%u candidate=%u expected=0x%x affected=0x%x attempts=%u receiver-restarts=held",
        recovery.reason, recovery.old, recovery.candidate, expected, affected, recovery.attempts);
    log_samples("before");
    reset_samples(now);
    kms_link_channel_recovery_pause();
}
static bool improved(uint8_t online)
{
    if ((online & recovery.expected) != recovery.expected) return false;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        uint8_t bit = slot_bit(i + 1);
        if (!(recovery.expected & bit)) continue;
        const samples_t *s = &recovery.peers[i].samples, *old = &recovery.baseline[i];
        bool affected = (recovery.affected & bit) != 0;
        if (s->replies < (affected ? MIN_SAMPLES : 2) || s->bad * 4 > s->samples || s->timeouts * 4 > s->samples)
            return false;
        unsigned bad_rate = 100u * s->bad / s->samples;
        unsigned baseline_rate = old->samples ? 100u * old->bad / old->samples : 0;
        if (affected && bad_rate + 25u > baseline_rate) return false;
        if (!affected && old->samples >= MIN_SAMPLES && bad_rate > baseline_rate + 10u) return false;
    }
    return true;
}
static void start_rollback(uint32_t now, enum result outcome)
{
    if (recovery.phase == VERIFY) log_samples("candidate");
    recovery.rollback_result = outcome;
    recovery.interrupt = NO_RESULT;
    recovery.stage_at = now;
    reset_samples(now);
    kms_radio_channel_trial_cancel_switch();
    if (kms_radio_channel_get() == recovery.old) recovery.phase = RECONNECT;
    else if (kms_radio_channel_trial_switch(recovery.old)) recovery.phase = ROLLBACK;
    else { finish(FAILED, false, now); return; }
    ESP_LOGI(TAG, "KMSCHANNELRECOVERY rollback channel=%u reason=%u", recovery.old, outcome);
}
static void trial_tick(uint32_t now, bool observe, bool idle, bool driver_blocked)
{
    if (!kms_radio_channel_trial_active()) { finish(MANUAL, false, now); return; }
    if (driver_blocked) { finish(FAILED, false, now); return; }
    uint32_t elapsed = now - recovery.stage_at;
    if (recovery.interrupt && recovery.phase != RECONNECT) {
        if (recovery.phase == ROLLBACK && recovery.rollback_retried) { finish(FAILED, false, now); return; }
        recovery.rollback_retried = recovery.phase == ROLLBACK;
        start_rollback(now, recovery.interrupt);
        return;
    }
    if (recovery.phase == ANNOUNCE || recovery.phase == ROLLBACK) {
        if (kms_radio_channel_switch_pending() && elapsed < SWITCH_LIMIT_MS) return;
        unsigned channel = kms_radio_channel_get();
        if (recovery.phase == ANNOUNCE && channel != recovery.candidate) { start_rollback(now, FAILED); return; }
        if (recovery.phase == ROLLBACK && channel != recovery.old) { finish(FAILED, false, now); return; }
        recovery.phase = recovery.phase == ANNOUNCE ? VERIFY : RECONNECT;
        recovery.stage_at = now;
        return;
    }
    uint8_t online = reachable(now, true);
    if (observe) take_sample(now);
    else recovery.probe.token = 0;
    if (recovery.phase == VERIFY) {
        if (observe && improved(online)) {
            if (kms_radio_channel_trial_commit()) { log_samples("candidate"); finish(KEPT, true, now); }
            else start_rollback(now, STORAGE);
            return;
        }
        if (elapsed >= VERIFY_MS) { start_rollback(now, REVERTED); return; }
    } else {
        uint8_t replied = 0;
        for (unsigned i = 0; i < KMS_SLOTS; ++i) if (recovery.peers[i].samples.replies) replied |= slot_bit(i + 1);
        if (((online & replied) & recovery.expected) == recovery.expected || elapsed >= RECONNECT_MS) {
            log_samples("restored");
            finish(recovery.rollback_result, true, now);
            return;
        }
    }
    if (observe && idle) send_probe(now, online & recovery.expected);
}
void kms_channel_recovery_tick(uint32_t now, unsigned selected, bool observe, bool idle, bool driver_blocked)
{
    if (recovery.phase != OBSERVE) { trial_tick(now, observe, idle, driver_blocked); return; }
    if (recovery.cooling && now - recovery.cooldown_at >= (uint32_t)recovery.cooldown * 60000u) recovery.cooling = false;
    if (recovery.attempts && now - recovery.budget_at >= BUDGET_MS) { recovery.attempts = 0; recovery.tried = 0; }
    uint8_t expected = (recovery.settings & ENABLED) && recovery.storage_ok && observe &&
                       !driver_blocked && !kms_radio_channel_recovering() ? reachable(now, false) : 0;
    if (expected && expected != reachable(now, true)) expected = 0;
    if (expected != recovery.observed) {
        reset_samples(now);
        recovery.solo_windows = 0;
        recovery.observed = expected;
    }
    if (!expected) return;
    take_sample(now);
    bool solo = only_one(expected);
    bool expired = now - recovery.window_at >= OBSERVE_MS;
    uint8_t bad = degraded(now, expected, solo);
    if (solo && expired) recovery.solo_windows = bad ? recovery.solo_windows + (recovery.solo_windows < SOLO_WINDOWS) : 0;
    bool qualifies = solo ? (recovery.settings & SINGLE_PEER) && recovery.solo_windows >= SOLO_WINDOWS && bad :
                            (bad & (bad - 1)) != 0;
    if (qualifies && idle && selected && (expected & slot_bit(selected))) {
        begin_trial(now, expected, bad, solo);
        if (recovery.phase != OBSERVE) return;
    }
    if (expired) reset_samples(now);
    if (idle) send_probe(now, expected);
}
void kms_channel_recovery_cancel(void)
{
    if (recovery.phase == ANNOUNCE || recovery.phase == VERIFY) {
        recovery.interrupt = CANCELLED;
        kms_radio_channel_trial_cancel_switch();
    }
}
void kms_channel_recovery_driver_stall(void)
{
    if (recovery.phase == OBSERVE) return;
    recovery.interrupt = DRIVER;
    kms_radio_channel_trial_cancel_switch();
}
void kms_channel_recovery_command(kms_desktop_message_t *m)
{
    if (m->op == KMS_D_CHANNEL_RECOVERY_SET) {
        unsigned settings = m->data[1], cooldown = m->data[2];
        if (m->data[0] != SCHEMA || !valid_settings(settings, cooldown)) { m->result = KMS_D_INVALID; return; }
        for (unsigned i = 3; i < KMS_D_DATA; ++i) if (m->data[i]) { m->result = KMS_D_INVALID; return; }
        bool changed = settings != recovery.settings || cooldown != recovery.cooldown;
        if (!(settings & ENABLED) && (recovery.settings & ENABLED)) {
            recovery.settings &= (uint8_t)~ENABLED;
            recovery.settings_dirty = true;
            kms_link_channel_recovery_disown();
            kms_channel_recovery_cancel();
        }
        if (!recovery.storage_ok ||
            ((changed || recovery.settings_dirty) && !save(settings, cooldown, recovery.attempts, recovery.tried))) {
            m->result = KMS_D_STORAGE;
            return;
        }
        recovery.settings_dirty = false;
        if (changed) {
            recovery.solo_windows = 0;
            if (recovery.phase == OBSERVE) reset_samples(kms_now());
            if (recovery.reason == SINGLE_LINK && !(settings & SINGLE_PEER)) kms_channel_recovery_cancel();
        }
        recovery.settings = (uint8_t)settings;
        recovery.cooldown = (uint8_t)cooldown;
    }
    memset(m->data, 0, sizeof(m->data));
    m->data[0] = SCHEMA; m->data[1] = recovery.settings; m->data[2] = recovery.cooldown;
    m->data[3] = (uint8_t)recovery.phase; m->data[4] = (uint8_t)recovery.last;
    m->data[5] = recovery.old; m->data[6] = recovery.candidate; m->data[7] = (uint8_t)recovery.reason;
    m->data[8] = recovery.expected; m->data[9] = recovery.affected; m->data[10] = recovery.attempts;
    m->data[11] = MAX_ATTEMPTS;
}
