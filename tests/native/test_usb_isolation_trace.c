#include "driver_trace.h"
#include <string.h>
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_run_driver_trace_tests(void)
{
    static iso_trace_store_t s, frozen;
    static iso_trace_sample_t sample;
    checks = 0; iso_trace_reset(&s);
    CHECK(!s.frozen && !s.event_count && !s.sample_count && !s.isrs);
    // Full ring preserves exactly the newest events in chronological order.
    for (unsigned n = 0; n < ISO_TRACE_EVENTS + 17; ++n) {
        iso_trace_event_t e = {n, n, n % 5, n % 8, n, 0, 0, 0, 0, 0, 0}; iso_trace_event(&s, &e);
    }
    CHECK(s.event_count == ISO_TRACE_EVENTS && s.overwritten == 17);
    for (unsigned n = 0; n < ISO_TRACE_EVENTS; ++n) {
        unsigned index = (s.event_next + n) % ISO_TRACE_EVENTS;
        CHECK(s.events[index].at == n + 17);
    }
    uint32_t total = 0;
    for (unsigned ch = 0; ch < 8; ++ch) for (unsigned k = 0; k < 5; ++k) total += s.counts[ch][k];
    CHECK(total == ISO_TRACE_EVENTS + 17);
    frozen = s;
    iso_trace_event_t invalid = {.channel = 8}; iso_trace_event(&s, &invalid);
    invalid.channel = 0; invalid.kind = 5; iso_trace_event(&s, &invalid);
    CHECK(!memcmp(&s, &frozen, sizeof(s)));
    // Root IRQ counters cannot flood out the transfer history.
    for (unsigned n = 0; n < 10000; ++n) iso_trace_isr(&s, n);
    CHECK(s.isrs == 10000 && s.last_isr == 9999 && s.event_count == ISO_TRACE_EVENTS);
    for (unsigned n = 0; n < 9; ++n) {
        sample.at = n; sample.channel[3].status = n; iso_trace_sample(&s, &sample);
    }
    CHECK(s.sample_count == 6 && s.sample_next == 3);
    for (unsigned n = 0; n < 6; ++n) {
        const iso_trace_sample_t *r = &s.samples[(s.sample_next + n) % 6];
        CHECK(r->at == n + 3 && r->channel[3].status == n + 3 && r->isrs == 10000);
    }
    iso_trace_freeze(&s); frozen = s;
    iso_trace_event_t e = {.at = 123, .kind = ISO_TRACE_DEQUEUE, .channel = 3};
    iso_trace_event(&s, &e); iso_trace_sample(&s, &sample); iso_trace_isr(&s, 123); iso_trace_freeze(&s);
    CHECK(!memcmp(&s, &frozen, sizeof(s))); // Late completion cannot replace first-fault evidence.
    iso_trace_reset(&s); CHECK(!s.frozen && !s.counts[3][ISO_TRACE_DEQUEUE] && !s.event_count);
    // uint32 counters/timestamps wrap as unsigned values, without storage corruption.
    s.isrs = 0xffffffffu; iso_trace_isr(&s, 0);
    CHECK(!s.isrs && !s.last_isr);
    s.counts[3][ISO_TRACE_DEQUEUE] = 0xffffffffu; iso_trace_event(&s, &e);
    CHECK(!s.counts[3][ISO_TRACE_DEQUEUE] && s.event_count == 1);
    return 0;
}
__declspec(dllexport) unsigned kms_driver_trace_check_count(void) { return checks; }
