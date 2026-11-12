#include "kms_diagnostics.h"
#include <string.h>

const char *kms_event_name(unsigned e)
{
    static const char *const names[] = {
        "boot", "acknowledgement timeout", "receiver lost", "receiver restarted",
        "receiver USB unavailable", "transmit queue overflow", "radio receive overflow",
        "USB output stalled", "input lease expired", "input disconnected", "input error",
        "input queue overflow", "keyboard LED failed", "keyboard LED pending",
        "buffered input discarded", "connection recovered", "selection unavailable", "computer selected"
    };
    return e < KMS_EVENT_COUNT ? names[e] : "unknown";
}
const char *kms_diag_event_name(unsigned event, uint32_t detail)
{
    if (event == KMS_EVENT_INPUT_ERROR && (detail & 0xff000000u) == KMS_USB_FAULT_EVENT_TAG) {
        static const char *const fields[] = {"USB fault operation/address", "USB fault address/interface", "USB fault pending controls",
            "USB root port low", "USB root port high", "USB frame age", "USB fault request", "USB fault request value",
            "USB fault request index", "USB fault device VID", "USB fault device PID"};
        unsigned field = (detail >> 16) & 255u;
        if (field >= KMS_USB_FAULT_OPERATION && field <= KMS_USB_FAULT_PID) return fields[field - 1];
    }
    if (event == KMS_EVENT_INPUT_ERROR && (detail & 0xff000000u) == KMS_USB_HOST_EVENT_TAG) {
        static const char *const names[] = {"USB control deadline", "USB host reconnect",
            "USB host recovered", "USB recovery blocked", "USB recovery restart", "USB hub absent", "USB call blocked"};
        unsigned phase = (detail >> 16) & 255u;
        if (phase >= KMS_USB_HOST_OVERDUE && phase <= KMS_USB_HOST_CALL_TIMEOUT) return names[phase - 1];
    }
    return kms_event_name(event);
}
void kms_diag_model_init(kms_diag_model_t *d, uint32_t now, uint32_t boot, uint32_t reset)
{
    memset(d, 0, sizeof(*d));
    d->last_tick = d->last_input = d->checkpoint = now;
    d->stats.version = 1; d->stats.boot = boot; d->stats.reset_reason = reset;
}
void kms_diag_model_tick(kms_diag_model_t *d, uint32_t now)
{
    uint32_t elapsed = now - d->last_tick;
    uint32_t idle = now - d->last_input;
    d->stats.uptime_ms += elapsed;
    if (!d->held && idle >= KMS_DIAGNOSTICS_IDLE_MS) {
        uint32_t part = idle - KMS_DIAGNOSTICS_IDLE_MS;
        d->stats.idle_ms += part < elapsed ? part : elapsed;
    }
    if (!d->held && idle > d->stats.max_idle_ms) d->stats.max_idle_ms = idle;
    d->last_tick = now;
}
void kms_diag_model_input(kms_diag_model_t *d, uint32_t now, bool changed, bool held)
{
    kms_diag_model_tick(d, now); ++d->stats.reports;
    if (changed) { ++d->stats.activity; d->last_input = now; }
    d->held = held;
}
unsigned kms_diag_model_event(kms_diag_model_t *d, uint32_t now, enum kms_event e, unsigned slot, uint32_t detail)
{
    if ((unsigned)e >= KMS_EVENT_COUNT) return KMS_DIAGNOSTICS_EVENTS;
    kms_diag_model_tick(d, now); ++d->stats.events[e];
    unsigned last = (d->next + KMS_DIAGNOSTICS_EVENTS - 1) % KMS_DIAGNOSTICS_EVENTS;
    kms_diag_record_t *r = &d->records[last];
    if (r->version && r->boot == d->stats.boot && r->event == e && r->slot == slot && r->detail == detail &&
        !kms_time_elapsed(now, r->uptime_ms, KMS_DIAGNOSTICS_REPEAT_MS)) {
        ++r->repeats; return last;
    }
    unsigned index = d->next; r = &d->records[index];
    if (!++d->next_sequence) ++d->next_sequence;
    *r = (kms_diag_record_t){.version = 1, .sequence = d->next_sequence,
        .boot = d->stats.boot, .uptime_ms = now, .idle_ms = d->held ? 0 : now - d->last_input,
        .detail = detail, .repeats = 1, .event = (uint16_t)e, .slot = (uint16_t)slot};
    d->next = (index + 1) % KMS_DIAGNOSTICS_EVENTS;
    return index;
}
bool kms_diag_model_checkpoint(kms_diag_model_t *d, uint32_t now)
{
    if (!kms_time_elapsed(now, d->checkpoint, KMS_DIAGNOSTICS_STATS_MS)) return false;
    kms_diag_model_tick(d, now); d->checkpoint = now; return true;
}
