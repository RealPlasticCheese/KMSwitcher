#include "driver_trace.h"
#include <inttypes.h>
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/usb_struct.h"

static iso_trace_store_t store;
static portMUX_TYPE trace_lock = portMUX_INITIALIZER_UNLOCKED;
void iso_driver_reset(void)
{
    portENTER_CRITICAL_SAFE(&trace_lock); iso_trace_reset(&store); portEXIT_CRITICAL_SAFE(&trace_lock);
}
void iso_driver_record(uint32_t kind, uint32_t channel, uint32_t object,
                       uint32_t detail, uint32_t qtd, uint32_t status)
{
    iso_trace_event_t event = {(uint32_t)esp_timer_get_time(), USB0.hfnum,
        kind, channel, object, detail, qtd, status, 0, 0, 0};
    // HCD hooks execute in task and ISR contexts; SAFE selects the matching
    // critical primitive. Always lock HCD before trace, never reverse.
    portENTER_CRITICAL_SAFE(&trace_lock); iso_trace_event(&store, &event); portEXIT_CRITICAL_SAFE(&trace_lock);
}
void iso_driver_activation(uint32_t channel, uint32_t object, uint32_t detail,
                           uint32_t qtd, uint32_t status, uint32_t before)
{
    iso_trace_event_t event = {(uint32_t)esp_timer_get_time(), USB0.hfnum,
        ISO_TRACE_ACTIVATE, channel, object, detail, qtd, status, before, 0, 0};
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (channel < ISO_TRACE_CHANNELS) { event.edge_before=store.edge_before[channel]; event.edge_after=store.edge_after[channel]; }
    iso_trace_event(&store, &event); portEXIT_CRITICAL_SAFE(&trace_lock);
}
void iso_driver_enable_edge(uint32_t channel, uint32_t before, uint32_t after)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!store.frozen && channel < ISO_TRACE_CHANNELS) { store.edge_before[channel]=before; store.edge_after[channel]=after; }
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void iso_driver_isr(void)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!store.frozen) { store.root_status=USB0.gintsts; store.root_mask=USB0.gintmsk; store.root_channels=USB0.haint; }
    iso_trace_isr(&store, (uint32_t)esp_timer_get_time());
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void iso_driver_isr_exit(void)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!store.frozen) {
        ++store.exits; uint32_t elapsed=(uint32_t)esp_timer_get_time()-store.last_isr;
        if (elapsed>store.max_isr) store.max_isr=elapsed;
    }
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void iso_driver_sample(const iso_trace_sample_t *sample, bool freeze)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    iso_trace_sample(&store, sample);
    if (freeze) iso_trace_freeze(&store);
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void iso_driver_print(void)
{
    // Only immutable first-fault storage may be printed. No long critical section.
    portENTER_CRITICAL_SAFE(&trace_lock); bool frozen = store.frozen; portEXIT_CRITICAL_SAFE(&trace_lock);
    if (!frozen) return;
    unsigned buffer_records=0;
    for (unsigned n=0;n<ISO_TRACE_SAMPLES;++n) for (unsigned ch=0;ch<ISO_TRACE_CHANNELS;++ch)
        if (store.samples[n].channel[ch].present) buffer_records+=2;
    printf("KMSISO TRACE version=1 bytes=%u events=%" PRIu32 " overwritten=%" PRIu32
           " samples=%" PRIu32 " isrs=%" PRIu32 " last-isr=%" PRIu32 " exits=%" PRIu32
           " max-isr=%" PRIu32 " root-status=%08" PRIx32 " root-mask=%08" PRIx32
           " root-channels=%08" PRIx32 " buffers=%u frozen=1\n",
           (unsigned)sizeof(store), store.event_count, store.overwritten, store.sample_count, store.isrs, store.last_isr, store.exits, store.max_isr, store.root_status, store.root_mask, store.root_channels, buffer_records);
    for (unsigned ch = 0; ch < ISO_TRACE_CHANNELS; ++ch) {
        const uint32_t *c = store.counts[ch];
        printf("KMSISO TCOUNT channel=%u enqueued=%" PRIu32 " activated=%" PRIu32
               " irqs=%" PRIu32 " parsed=%" PRIu32 " dequeued=%" PRIu32 "\n", ch, c[0], c[1], c[2], c[3], c[4]);
    }
    unsigned first = (store.event_next + ISO_TRACE_EVENTS - store.event_count) % ISO_TRACE_EVENTS;
    for (unsigned n = 0; n < store.event_count; ++n) {
        const iso_trace_event_t *e = &store.events[(first + n) % ISO_TRACE_EVENTS];
        printf("KMSISO TEVENT at=%" PRIu32 " frame=%08" PRIx32 " kind=%" PRIu32
               " channel=%" PRIu32 " object=%08" PRIx32 " detail=%08" PRIx32
               " qtd=%08" PRIx32 " status=%08" PRIx32 " before=%08" PRIx32 " edge-before=%08" PRIx32 " edge-after=%08" PRIx32 "\n", e->at, e->frame, e->kind,
               e->channel, e->object, e->detail, e->qtd, e->status, e->before, e->edge_before, e->edge_after);
    }
    first = (store.sample_next + ISO_TRACE_SAMPLES - store.sample_count) % ISO_TRACE_SAMPLES;
    for (unsigned n = 0; n < store.sample_count; ++n) {
        const iso_trace_sample_t *s = &store.samples[(first + n) % ISO_TRACE_SAMPLES];
        printf("KMSISO TSAMPLE n=%u at=%" PRIu32 " frame=%08" PRIx32 " core=%08" PRIx32
               " mask=%08" PRIx32 " channels=%08" PRIx32 " isrs=%" PRIu32 " last-isr=%" PRIu32 "\n",
               n, s->at, s->frame, s->core, s->mask, s->channels, s->isrs, s->last_isr);
        for (unsigned ch = 0; ch < ISO_TRACE_CHANNELS; ++ch) {
            const iso_trace_channel_t *c = &s->channel[ch]; if (!c->present) continue;
            printf("KMSISO TPIPE sample=%u channel=%u pipe=%08" PRIx32 " state=%" PRIu32
                   " endpoint=%08" PRIx32 " hal=%08" PRIx32 " multi=%08" PRIx32
                   " pending=%" PRIu32 " done=%" PRIu32 " char=%08" PRIx32 " irq=%08" PRIx32
                   " mask=%08" PRIx32 " size=%08" PRIx32 " dma=%08" PRIx32 " index=%" PRIu32
                   " list=%08" PRIx32 " urb=%08" PRIx32 " qtd=%08" PRIx32 " status=%08" PRIx32
                   " data=%08" PRIx32 " schedule-current=%08" PRIx32 " schedule-next=%08" PRIx32
                   " schedule-mask=%08" PRIx32 "\n", n, ch, c->pipe, c->state, c->endpoint, c->hal,
                   c->multi, c->pending, c->done, c->hcchar, c->irq, c->mask, c->size, c->dma, c->index,
                   c->list, c->urb, c->qtd, c->status, c->data, c->schedule_current, c->schedule_next, c->schedule_mask);
            for (unsigned slot=0;slot<2;++slot) {
                const iso_trace_buffer_t *b=&c->buffers[slot];
                printf("KMSISO TBUFFER sample=%u channel=%u slot=%u list=%08" PRIx32 " urb=%08" PRIx32
                       " flags=%08" PRIx32 " state=%08" PRIx32 " first=%08" PRIx32 " second=%08" PRIx32
                       " index=%" PRIu32 " current=%08" PRIx32 " data=%08" PRIx32 "\n",n,ch,slot,
                       b->list,b->urb,b->flags,b->state,b->first,b->second,b->index,b->current,b->data);
            }
        }
    }
}
