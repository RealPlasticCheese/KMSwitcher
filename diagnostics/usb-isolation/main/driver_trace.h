#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { ISO_TRACE_CHANNELS = 8, ISO_TRACE_EVENTS = 128, ISO_TRACE_SAMPLES = 6 };
enum { ISO_TRACE_ENQUEUE, ISO_TRACE_ACTIVATE, ISO_TRACE_IRQ, ISO_TRACE_PARSED,
       ISO_TRACE_DEQUEUE, ISO_TRACE_EVENT_KINDS };

typedef struct {
    uint32_t at, frame, kind, channel, object, detail, qtd, status, before, edge_before, edge_after;
} iso_trace_event_t;
typedef struct {
    uint32_t list, urb, flags, state, first, second, index, current, data;
} iso_trace_buffer_t;
typedef struct {
    uint32_t present, pipe, state, endpoint, hal, multi, pending, done;
    uint32_t hcchar, irq, mask, size, dma, index, list, urb, qtd, status, data;
    uint32_t schedule_current, schedule_next, schedule_mask;
    iso_trace_buffer_t buffers[2];
} iso_trace_channel_t;
typedef struct {
    uint32_t at, frame, core, mask, channels;
    uint32_t isrs, last_isr;
    iso_trace_channel_t channel[ISO_TRACE_CHANNELS];
} iso_trace_sample_t;
typedef struct {
    iso_trace_event_t events[ISO_TRACE_EVENTS];
    iso_trace_sample_t samples[ISO_TRACE_SAMPLES];
    uint32_t counts[ISO_TRACE_CHANNELS][ISO_TRACE_EVENT_KINDS];
    uint32_t event_next, event_count, sample_next, sample_count, overwritten;
    uint32_t isrs, last_isr, exits, max_isr, root_status, root_mask, root_channels;
    uint32_t edge_before[ISO_TRACE_CHANNELS], edge_after[ISO_TRACE_CHANNELS];
    bool frozen;
} iso_trace_store_t;

// Pure storage operations: callers serialize access, including reset/freeze.
void iso_trace_reset(iso_trace_store_t *store);
void iso_trace_event(iso_trace_store_t *store, const iso_trace_event_t *event);
void iso_trace_isr(iso_trace_store_t *store, uint32_t at);
void iso_trace_sample(iso_trace_store_t *store, const iso_trace_sample_t *sample);
void iso_trace_freeze(iso_trace_store_t *store);

// ESP32-S3 platform adapter. No allocation or UART writes in capture hooks.
void iso_driver_reset(void);
void iso_driver_record(uint32_t kind, uint32_t channel, uint32_t object,
                       uint32_t detail, uint32_t qtd, uint32_t status);
void iso_driver_activation(uint32_t channel, uint32_t object, uint32_t detail,
                           uint32_t qtd, uint32_t status, uint32_t before);
void iso_driver_enable_edge(uint32_t channel, uint32_t before, uint32_t after);
void iso_driver_isr(void);
void iso_driver_isr_exit(void);
void iso_driver_sample(const iso_trace_sample_t *sample, bool freeze);
void iso_driver_print(void);
// Implemented inside the instrumented HCD, under its existing lock.
void iso_hcd_capture(bool freeze);
