// Exercise the production guard with a deterministic frame clock. This models
// the observed boundary trigger; it does not emulate the controller's silicon.
#include <stdint.h>
static uint32_t guard_frame(void), guard_time(void);
#define USB0 guard_test_usb
#define ISO_GUARD_FRAME() guard_frame()
#define ISO_GUARD_TIME() guard_time()
#include "../../diagnostics/usb-isolation/main/frame_guard.c"
usb_dev_t guard_test_usb;
static uint64_t ticks;
static uint32_t time_us, reads;
static bool frame_stopped, timer_stopped;
static uint32_t guard_frame(void)
{
    ++reads;
    uint32_t interval=USB0.hfir ? USB0.hfir : 48000;
    uint32_t value=((interval-(uint32_t)(ticks%interval))<<16)|((uint32_t)(ticks/interval)&0xffff);
    if (!frame_stopped) ticks+=4;
    return value;
}
static uint32_t guard_time(void)
{
    if (!timer_stopped) { ++time_us; if (!frame_stopped) ticks+=USB0.hfir/1000; }
    return time_us;
}
static unsigned checks, scenarios;
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static void reset_guard(bool enabled, unsigned interval, unsigned remaining)
{
    USB0.hfir=interval; ticks=(uint64_t)100*interval+interval-remaining;
    time_us=reads=0; frame_stopped=timer_stopped=false; iso_guard_configure(enabled);
}
static bool activate(bool eligible)
{
    iso_guard_before_enable(eligible);
    uint32_t before=guard_frame(); ticks+=16; uint32_t after=guard_frame();
    iso_guard_after_enable(eligible,before,after);
    return (before&0xffff)!=(after&0xffff);
}
__declspec(dllexport) int kms_run_frame_guard_tests(void)
{
    checks=scenarios=0;
    // Captured FRREM=17 failure: the control crosses the boundary; the guard
    // starts in the next frame's safe window without changing the transfer.
    reset_guard(false,48000,17); CHECK(activate(true)); CHECK(guard.crossings==1);
    reset_guard(true,48000,17); CHECK(!activate(true));
    CHECK(guard.waited==1 && !guard.timeouts && !guard.crossings && guard.max_wait_us<=64);
    // Later direct-connected control failed at FRREM=15, without a USB hub.
    reset_guard(false,48000,15); CHECK(activate(true) && guard.crossings==1);
    reset_guard(true,48000,15); CHECK(!activate(true) && guard.waited==1 && !guard.timeouts);
    // Exhaust every phase at both S3 full/low-speed frame clocks and both
    // switch states, including endpoints outside the workaround's scope.
    const unsigned intervals[]={48000,6000};
    for (unsigned clock=0;clock<2;++clock) for (unsigned phase=1;phase<=intervals[clock];++phase)
        for (unsigned enabled=0;enabled<2;++enabled) for (unsigned eligible=0;eligible<2;++eligible) {
            ++scenarios; reset_guard(enabled!=0,intervals[clock],phase);
            uint64_t original=ticks; bool crossed=activate(eligible!=0);
            CHECK(!guard.timeouts && guard.max_wait_us<ISO_GUARD_BUDGET_US);
            CHECK(guard.activations==eligible);
            if (enabled && eligible) CHECK(!crossed);
            else CHECK(ticks==original+24); // Disabled/unrelated endpoints never wait or sample time.
        }
    // A frozen frame clock or timer cannot spin forever inside a critical section.
    reset_guard(true,48000,17); frame_stopped=true; iso_guard_before_enable(true);
    CHECK(guard.timeouts==1 && guard.max_wait_us==64 && reads<=ISO_GUARD_MAX_READS+1);
    reset_guard(true,48000,17); frame_stopped=timer_stopped=true; iso_guard_before_enable(true);
    CHECK(guard.timeouts==1 && reads==ISO_GUARD_MAX_READS+1);
    // Invalid HFIR is observable and leaves normal SDK activation available.
    reset_guard(true,0,17); iso_guard_before_enable(true); CHECK(guard.timeouts==1 && reads==0);
    reset_guard(true,48000,17); time_us=0xfffffff0u; iso_guard_before_enable(true);
    CHECK(guard.waited==1 && !guard.timeouts && guard.max_wait_us<64);
    // Reset retains the selected mode while clearing per-run evidence.
    iso_guard_reset(); CHECK(guard.enabled==1 && !guard.activations && !guard.waited);
    iso_guard_configure(false); iso_guard_stats_t snapshot; iso_guard_snapshot(&snapshot);
    CHECK(!snapshot.enabled && !snapshot.activations);
    return 0;
}
__declspec(dllexport) unsigned kms_frame_guard_check_count(void) { return checks; }
__declspec(dllexport) unsigned kms_frame_guard_scenario_count(void) { return scenarios; }
