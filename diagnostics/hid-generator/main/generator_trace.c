#include "generator_trace.h"
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/usb_struct.h"

enum { EVENTS = 64, SAMPLES = 6 };
typedef struct {
    uint32_t at, kind, status, mask, device, endpoints, ctl, irq, size, dma, fifo, ahb, detail, value;
    uint32_t endpoint_mask, in_mask, device_control;
} sample_t;
typedef struct {
    sample_t events[EVENTS], samples[SAMPLES];
    uint32_t next, count, overwritten, sample_next, sample_count, last_sample;
    uint32_t run, entries, exits, last_enter, max_isr, arms, accepted, callbacks, resets, suspended, resumed;
    bool frozen;
} trace_t;
static trace_t trace;
static portMUX_TYPE trace_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
static sample_t sample(uint32_t kind, uint32_t detail, uint32_t value)
{
    // Ordinary status reads only: never pop RX data, touch FIFO payload or acknowledge IRQs.
    return (sample_t){now_us(),kind,USB0.gintsts,USB0.gintmsk,USB0.dsts,USB0.daint,
        USB0.in_ep_reg[1].diepctl,USB0.in_ep_reg[1].diepint,USB0.in_ep_reg[1].dieptsiz,
        USB0.in_ep_reg[1].diepdma,USB0.in_ep_reg[1].dtxfsts,USB0.gahbcfg,detail,value,
        USB0.daintmsk,USB0.diepmsk,USB0.dctl};
}
static void record(uint32_t kind, uint32_t detail, uint32_t value)
{
    if (trace.frozen) return;
    trace.events[trace.next] = sample(kind, detail, value);
    trace.next = (trace.next + 1) % EVENTS;
    if (trace.count < EVENTS) ++trace.count; else ++trace.overwritten;
}
static void periodic(void)
{
    trace.samples[trace.sample_next] = sample(4,0,0);
    trace.sample_next = (trace.sample_next + 1) % SAMPLES;
    if (trace.sample_count < SAMPLES) ++trace.sample_count;
}
void kms_gen_trace_irq_enter(uint32_t status)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!trace.frozen) {
        ++trace.entries; trace.last_enter = now_us();
        if (status & (1u << 12)) ++trace.resets;
        if (status & (1u << 11)) ++trace.suspended;
        if (status & (1u << 31)) ++trace.resumed;
        if (USB0.in_ep_reg[1].diepint || (status & ((1u << 12)|(1u << 11)|(1u << 31)))) record(1,status,0);
    }
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void kms_gen_trace_irq_exit(void)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!trace.frozen) {
        ++trace.exits; uint32_t elapsed = now_us() - trace.last_enter;
        if (elapsed > trace.max_isr) trace.max_isr = elapsed;
    }
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void kms_gen_trace_arm(const void *buffer, uint32_t bytes, uint32_t packet)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!trace.frozen) { ++trace.arms; record(0,(uint32_t)(uintptr_t)buffer,(bytes << 16)|(packet & 0xffff)); }
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void kms_gen_trace_mark(uint32_t kind, uint32_t value)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!trace.frozen) {
        if (kind == 2) ++trace.accepted; else if (kind == 3) ++trace.callbacks;
        record(kind,0,value);
    }
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void kms_gen_trace_start(uint32_t run)
{
    portENTER_CRITICAL_SAFE(&trace_lock); memset(&trace,0,sizeof(trace)); trace.run = run;
    trace.last_sample = now_us(); periodic(); portEXIT_CRITICAL_SAFE(&trace_lock);
}
void kms_gen_trace_tick(void)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!trace.frozen && now_us() - trace.last_sample >= 500000) {
        trace.last_sample = now_us(); periodic();
    }
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
void kms_gen_trace_freeze(void)
{
    portENTER_CRITICAL_SAFE(&trace_lock);
    if (!trace.frozen) { periodic(); trace.frozen = true; }
    portEXIT_CRITICAL_SAFE(&trace_lock);
}
static void print_sample(const char *name, unsigned n, const sample_t *s)
{
    printf("KMSGEN %s n=%u at=%" PRIu32 " kind=%" PRIu32 " status=%08" PRIx32
           " mask=%08" PRIx32 " device=%08" PRIx32 " endpoints=%08" PRIx32 " ctl=%08" PRIx32
           " irq=%08" PRIx32 " size=%08" PRIx32 " dma=%08" PRIx32 " fifo=%08" PRIx32
           " ahb=%08" PRIx32 " detail=%08" PRIx32 " value=%08" PRIx32
           " endpoint-mask=%08" PRIx32 " in-mask=%08" PRIx32 " device-control=%08" PRIx32 "\n", name,n,s->at,s->kind,
           s->status,s->mask,s->device,s->endpoints,s->ctl,s->irq,s->size,s->dma,s->fifo,s->ahb,s->detail,s->value,
           s->endpoint_mask,s->in_mask,s->device_control);
}
void kms_gen_trace_print(uint32_t token, const char *identity)
{
    portENTER_CRITICAL_SAFE(&trace_lock); bool frozen = trace.frozen; portEXIT_CRITICAL_SAFE(&trace_lock);
    if (!frozen) { printf("KMSGEN ERROR trace not frozen\n"); return; }
    printf("KMSGEN TRACE id=%" PRIu32 " serial=%s version=1 run=%" PRIu32 " frozen=1 events=%" PRIu32
           " samples=%" PRIu32 " overwritten=%" PRIu32 " entries=%" PRIu32 " exits=%" PRIu32
           " last-enter=%" PRIu32 " max-isr=%" PRIu32 " arms=%" PRIu32 " accepted=%" PRIu32
           " callbacks=%" PRIu32 " resets=%" PRIu32 " suspended=%" PRIu32 " resumed=%" PRIu32 "\n",
           token,identity,trace.run,trace.count,trace.sample_count,trace.overwritten,trace.entries,trace.exits,
           trace.last_enter,trace.max_isr,trace.arms,trace.accepted,trace.callbacks,trace.resets,trace.suspended,trace.resumed);
    unsigned first = (trace.next + EVENTS - trace.count) % EVENTS;
    for (unsigned n=0;n<trace.count;++n) print_sample("GEVENT",n,&trace.events[(first+n)%EVENTS]);
    first = (trace.sample_next + SAMPLES - trace.sample_count) % SAMPLES;
    for (unsigned n=0;n<trace.sample_count;++n) print_sample("GSAMPLE",n,&trace.samples[(first+n)%SAMPLES]);
    printf("KMSGEN TRACEEND id=%" PRIu32 "\n",token);
}
