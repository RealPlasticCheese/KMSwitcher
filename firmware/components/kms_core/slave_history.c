#include "kms_slave_history.h"
#include <string.h>

uint32_t kms_slave_radio_detail(unsigned reason, unsigned op, uint32_t duration)
{
    if (duration > 65535u) duration = 65535u;
    return KMS_RADIO_FAULT_TAG | ((reason & 15u) << 24) | ((op & 255u) << 16) | duration;
}

const char *kms_slave_event_name(unsigned e)
{
    static const char *const names[] = {"boot", "USB mounted", "USB suspended", "USB resumed",
        "USB unmounted", "USB stalled", "USB queue overflow", "radio queue overflow",
        "input lease expired", "output cleared", "radio send failed", "update started",
        "update verified", "update aborted", "update confirmed", "history storage failed"};
    return e < KMS_S_EVENT_COUNT ? names[e] : "unknown";
}
void kms_slave_history_clear(kms_slave_history_t *h)
{
    // The live statistics and totals deliberately survive a history clear.
    uint32_t sequence = h->journal.next_sequence;
    memset(&h->journal, 0, sizeof(h->journal));
    h->journal.version = 2; h->journal.event_capacity = KMS_SLAVE_HISTORY_EVENTS;
    h->journal.stats_capacity = KMS_SLAVE_HISTORY_STATS;
    h->journal.next_sequence = sequence;
}
void kms_slave_history_init(kms_slave_history_t *h, uint32_t now, uint32_t boot, uint32_t reset)
{
    memset(h, 0, sizeof(*h)); kms_slave_history_clear(h);
    h->live.boot = boot; h->live.reset = reset;
    h->last_tick = h->checkpoint = now; h->live.metrics[KMS_S_MIN_HEAP] = UINT32_MAX;
}
bool kms_slave_history_valid(const kms_slave_journal_t *j)
{
    if (j->version != 2 || j->event_capacity != KMS_SLAVE_HISTORY_EVENTS ||
        j->stats_capacity != KMS_SLAVE_HISTORY_STATS || j->next_event >= KMS_SLAVE_HISTORY_EVENTS ||
        j->event_count > KMS_SLAVE_HISTORY_EVENTS || j->next_stats >= KMS_SLAVE_HISTORY_STATS ||
        j->stats_count > KMS_SLAVE_HISTORY_STATS) return false;
    for (unsigned i = 0; i < KMS_SLAVE_HISTORY_EVENTS; ++i)
        if (j->events[i].repeats && j->events[i].event >= KMS_S_EVENT_COUNT) return false;
    return true;
}
void kms_slave_history_tick(kms_slave_history_t *h, uint32_t now)
{ h->live.uptime_ms += (uint32_t)(now - h->last_tick); h->last_tick = now; }
bool kms_slave_history_event(kms_slave_history_t *h, uint32_t now, unsigned event, uint32_t detail)
{
    if (event >= KMS_S_EVENT_COUNT) return false;
    kms_slave_history_tick(h, now); ++h->live.totals[event];
    kms_slave_journal_t *j = &h->journal;
    unsigned previous = (j->next_event + KMS_SLAVE_HISTORY_EVENTS - 1) % KMS_SLAVE_HISTORY_EVENTS;
    kms_slave_event_record_t *r = &j->events[previous];
    if (j->event_count && r->boot == h->live.boot && r->event == event && r->detail == detail &&
        !kms_time_elapsed(now, r->last_ms, KMS_SLAVE_HISTORY_REPEAT_MS)) {
        ++r->repeats; r->last_ms = now; return false;
    }
    if (!++j->next_sequence) ++j->next_sequence;
    r = &j->events[j->next_event];
    *r = (kms_slave_event_record_t){j->next_sequence, h->live.boot, now, now, detail, 1, event};
    j->next_event = (j->next_event + 1) % KMS_SLAVE_HISTORY_EVENTS;
    if (j->event_count == KMS_SLAVE_HISTORY_EVENTS) ++j->events_overwritten;
    else ++j->event_count;
    return true;
}
void kms_slave_history_checkpoint(kms_slave_history_t *h)
{
    kms_slave_journal_t *j = &h->journal;
    ++h->live.sequence; j->snapshots[j->next_stats] = h->live;
    j->next_stats = (j->next_stats + 1) % KMS_SLAVE_HISTORY_STATS;
    if (j->stats_count == KMS_SLAVE_HISTORY_STATS) ++j->stats_overwritten;
    else ++j->stats_count;
}
