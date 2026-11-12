// Run the production bounded trace with fake read-only registers and clock.
#define USB0 generator_test_usb
#define esp_timer_get_time generator_trace_time
#include "../../diagnostics/hid-generator/main/generator_trace.c"
usb_dev_t generator_test_usb;
static uint32_t clock_us;
int64_t generator_trace_time(void) { return clock_us; }
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_run_generator_trace_tests(void)
{
    checks=0; clock_us=10; kms_gen_trace_start(123);
    CHECK(trace.run==123 && trace.sample_count==1 && !trace.frozen);
    USB0.gintsts=0x55; USB0.in_ep_reg[1].diepint=1; USB0.in_ep_reg[1].dtxfsts=27;
    for (unsigned i=0;i<80;++i) kms_gen_trace_arm((void *)(uintptr_t)(0x100+i),55,64);
    CHECK(trace.count==64 && trace.overwritten==16 && trace.arms==80);
    CHECK(trace.events[trace.next].detail==0x110 && trace.events[trace.next].fifo==27);
    CHECK(USB0.gintsts==0x55 && USB0.in_ep_reg[1].diepint==1); // No IRQ acknowledgement.
    kms_gen_trace_mark(2,55); kms_gen_trace_mark(3,55);
    CHECK(trace.accepted==1 && trace.callbacks==1);
    clock_us=0xfffffff0u; kms_gen_trace_irq_enter((1u<<12)|(1u<<11)|(1u<<31));
    clock_us=10; kms_gen_trace_irq_exit();
    CHECK(trace.entries==1 && trace.exits==1 && trace.max_isr==26);
    CHECK(trace.resets==1 && trace.suspended==1 && trace.resumed==1);
    for (unsigned i=1;i<=8;++i) { clock_us=i*500000; kms_gen_trace_tick(); }
    CHECK(trace.sample_count==6);
    kms_gen_trace_freeze(); static trace_t saved; memcpy(&saved,&trace,sizeof(trace));
    kms_gen_trace_arm(0,1,1); kms_gen_trace_mark(2,1); kms_gen_trace_irq_enter(0);
    kms_gen_trace_irq_exit(); kms_gen_trace_tick(); kms_gen_trace_freeze();
    CHECK(memcmp(&saved,&trace,sizeof(trace))==0);
    kms_gen_trace_start(456);
    CHECK(trace.run==456 && !trace.frozen && trace.count==0 && trace.entries==0 && trace.sample_count==1);
    return 0;
}
__declspec(dllexport) unsigned kms_generator_trace_check_count(void) { return checks; }
