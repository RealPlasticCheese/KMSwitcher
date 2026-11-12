#include "kms_slave_snapshot.h"
#include <string.h>

typedef struct {
    uint8_t *out;
    size_t used, capacity;
    uint32_t position, offset, crc;
    bool measure;
} writer_t;
static void character(writer_t *w, char c)
{
    if (w->measure) {
        w->crc ^= (uint8_t)c;
        for (unsigned bit = 0; bit < 8; ++bit)
            w->crc = (w->crc >> 1) ^ (0xedb88320u & (0u - (w->crc & 1u)));
    } else if (w->position >= w->offset && w->used < w->capacity) w->out[w->used++] = (uint8_t)c;
    ++w->position;
}
static void text(writer_t *w, const char *s)
{ while (*s) character(w, *s++); }
static void number(writer_t *w, uint64_t value)
{
    char digits[20]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) character(w, digits[--n]);
}
static void signed_number(writer_t *w, int32_t value)
{
    if (value < 0) { character(w, '-'); number(w, (uint64_t)-(int64_t)value); }
    else number(w, (uint32_t)value);
}
static void boot(writer_t *w, uint32_t value)
{
    for (unsigned n = 8; n; --n) character(w, "0123456789abcdef"[(value >> ((n - 1) * 4)) & 15]);
}
static void field(writer_t *w, const char *name, uint64_t value)
{ text(w, name); number(w, value); }
static void stats(writer_t *w, const kms_slave_stats_t *s)
{
    field(w, "stats seq=", s->sequence); text(w, " boot="); boot(w, s->boot);
    field(w, " reset=", s->reset); field(w, " uptime_ms=", s->uptime_ms);
    text(w, " rssi="); signed_number(w, s->rssi_last);
    text(w, " mean_rssi="); signed_number(w, s->rssi_mean);
    static const char *const names[] = {" radio_rx=", " radio_tx=", " duplicates=", " rejected=", " accepted=",
        " usb_completed=", " peak_rx_queue=", " peak_usb_queue=", " max_usb_ms=", " min_heap=", " usb_flags="};
    for (unsigned i = 0; i < KMS_S_METRIC_COUNT; ++i) field(w, names[i], s->metrics[i]);
    field(w, " radio_failed=", s->radio_failed);
    character(w, '\n');
    for (unsigned i = 0; i < KMS_S_EVENT_COUNT; ++i) {
        field(w, "total event=", i); field(w, " count=", s->totals[i]);
        text(w, " name="); text(w, kms_slave_event_name(i)); character(w, '\n');
    }
}
static void block(writer_t *w, const kms_slave_snapshot_t *s, unsigned n)
{
    const kms_slave_journal_t *j = &s->history.journal;
    if (n == 0) {
        char sha[65]; kms_hex_encode(s->identity.sha256, 32, sha);
        text(w, "KMSwitcher slave diagnostics git="); text(w, s->identity.git);
        text(w, " utc="); text(w, s->identity.utc); text(w, " sha256="); text(w, sha);
        field(w, " bytes=", s->identity.image_bytes); character(w, '\n');
    } else if (n == 1) {
        field(w, "retention events=", j->event_count); field(w, "/", KMS_SLAVE_HISTORY_EVENTS);
        field(w, " overwritten=", j->events_overwritten); field(w, " checkpoints=", j->stats_count);
        field(w, "/", KMS_SLAVE_HISTORY_STATS); field(w, " overwritten=", j->stats_overwritten); character(w, '\n');
    } else if (n - 2 < j->event_count) {
        unsigned at = (j->next_event + KMS_SLAVE_HISTORY_EVENTS - j->event_count + n - 2) % KMS_SLAVE_HISTORY_EVENTS;
        const kms_slave_event_record_t *e = &j->events[at];
        field(w, "event seq=", e->sequence); text(w, " boot="); boot(w, e->boot);
        field(w, " first_ms=", e->first_ms); field(w, " last_ms=", e->last_ms);
        field(w, " code=", e->event); field(w, " detail=", e->detail); field(w, " repeats=", e->repeats);
        bool radio_fault = e->event == KMS_S_RADIO_ERROR && (e->detail & 0xf0000000u) == KMS_RADIO_FAULT_TAG;
        if (radio_fault) {
            field(w, " radio_reason=", (e->detail >> 24) & 15u);
            field(w, " op=", (e->detail >> 16) & 127u);
            field(w, " storage_busy=", (e->detail >> 23) & 1u); field(w, " duration_ms=", e->detail & 65535u);
        }
        text(w, " name="); text(w, radio_fault ? "radio fault" : kms_slave_event_name(e->event)); character(w, '\n');
    } else {
        unsigned stat = n - 2 - j->event_count;
        if (stat < j->stats_count) {
            unsigned at = (j->next_stats + KMS_SLAVE_HISTORY_STATS - j->stats_count + stat) % KMS_SLAVE_HISTORY_STATS;
            stats(w, &j->snapshots[at]);
        } else stats(w, &s->history.live);
    }
}
bool kms_slave_snapshot_prepare(kms_slave_snapshot_t *s)
{
    s->size = s->crc = s->blocks = 0;
    if (!kms_slave_history_valid(&s->history.journal) ||
        s->identity.git[KMS_GIT_BYTES - 1] || s->identity.utc[KMS_UTC_BYTES - 1]) return false;
    writer_t w = {.measure = true, .crc = UINT32_MAX};
    s->blocks = 3 + s->history.journal.event_count + s->history.journal.stats_count;
    for (unsigned n = 0; n < s->blocks; ++n) { s->offsets[n] = w.position; block(&w, s, n); }
    s->offsets[s->blocks] = w.position;
    if (!w.position || w.position > KMS_SLAVE_HISTORY_BYTES) return false;
    s->size = w.position; s->crc = ~w.crc; return true;
}
size_t kms_slave_snapshot_read(const kms_slave_snapshot_t *s, uint32_t offset, void *out, size_t capacity)
{
    if (!s->size || offset >= s->size || !out || !capacity) return 0;
    unsigned low = 0, high = s->blocks;
    while (low + 1 < high) {
        unsigned mid = low + (high - low) / 2;
        if (s->offsets[mid] <= offset) low = mid; else high = mid;
    }
    writer_t w = {.out = out, .capacity = capacity, .offset = offset, .position = s->offsets[low]};
    for (unsigned n = low; n < s->blocks && w.used < capacity; ++n) block(&w, s, n);
    return w.used;
}
