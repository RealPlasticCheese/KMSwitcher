#include "driver_trace.h"
#include <string.h>

void iso_trace_reset(iso_trace_store_t *s) { memset(s, 0, sizeof(*s)); }
void iso_trace_freeze(iso_trace_store_t *s) { s->frozen = true; }
void iso_trace_isr(iso_trace_store_t *s, uint32_t at)
{
    if (s->frozen) return;
    ++s->isrs; s->last_isr = at;
}
void iso_trace_event(iso_trace_store_t *s, const iso_trace_event_t *event)
{
    if (s->frozen || event->channel >= ISO_TRACE_CHANNELS || event->kind >= ISO_TRACE_EVENT_KINDS) return;
    ++s->counts[event->channel][event->kind];
    s->events[s->event_next] = *event;
    s->event_next = (s->event_next + 1) % ISO_TRACE_EVENTS;
    if (s->event_count < ISO_TRACE_EVENTS) ++s->event_count;
    else ++s->overwritten;
}
void iso_trace_sample(iso_trace_store_t *s, const iso_trace_sample_t *sample)
{
    if (s->frozen) return;
    s->samples[s->sample_next] = *sample;
    s->samples[s->sample_next].isrs = s->isrs;
    s->samples[s->sample_next].last_isr = s->last_isr;
    s->sample_next = (s->sample_next + 1) % ISO_TRACE_SAMPLES;
    if (s->sample_count < ISO_TRACE_SAMPLES) ++s->sample_count;
}
