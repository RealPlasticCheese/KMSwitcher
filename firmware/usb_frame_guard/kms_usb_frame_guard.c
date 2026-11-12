// Experimental ESP32-S3 workaround. Never reset, halt or alter QTD ownership.
#include "kms_usb_frame_guard.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/usb_struct.h"

#ifndef KMS_USB_GUARD_FRAME
#define KMS_USB_GUARD_FRAME() USB0.hfnum
#endif
#ifndef KMS_USB_GUARD_TIME
#define KMS_USB_GUARD_TIME() ((uint32_t)esp_timer_get_time())
#endif

static DRAM_ATTR kms_usb_frame_guard_stats_t guard = {.enabled = 1};
static DRAM_ATTR portMUX_TYPE guard_lock = portMUX_INITIALIZER_UNLOCKED;

void kms_usb_frame_guard_configure(bool enabled)
{
    portENTER_CRITICAL_SAFE(&guard_lock);
    guard = (kms_usb_frame_guard_stats_t){.enabled = enabled};
    portEXIT_CRITICAL_SAFE(&guard_lock);
}
void kms_usb_frame_guard_reset(void)
{
    portENTER_CRITICAL_SAFE(&guard_lock);
    guard = (kms_usb_frame_guard_stats_t){.enabled = guard.enabled};
    portEXIT_CRITICAL_SAFE(&guard_lock);
}
void kms_usb_frame_guard_snapshot(kms_usb_frame_guard_stats_t *out)
{
    portENTER_CRITICAL_SAFE(&guard_lock);
    *out = guard;
    portEXIT_CRITICAL_SAFE(&guard_lock);
}
static bool IRAM_ATTR safe_phase(uint32_t frame, uint32_t interval, uint32_t margin)
{
    uint32_t remaining = frame >> 16;
    // HFNUM can briefly expose the new frame number before FRREM reloads.
    return remaining >= margin && remaining <= interval - margin;
}
void IRAM_ATTR kms_usb_frame_guard_before_enable(bool interrupt_in)
{
    if (!interrupt_in) return;
    // HCD already holds its critical section. Lock order is HCD -> guard;
    // configuration/snapshot never acquires the HCD lock while holding guard.
    portENTER_CRITICAL_SAFE(&guard_lock);
    ++guard.activations;
    if (guard.enabled) {
        uint32_t interval = USB0.hfir & 0xffff;
        uint32_t margin = interval * KMS_USB_GUARD_US / 1000;
        if (!margin || margin * 2 >= interval) ++guard.timeouts;
        else if (!safe_phase(KMS_USB_GUARD_FRAME(), interval, margin)) {
            ++guard.waited;
            uint32_t start = KMS_USB_GUARD_TIME(), elapsed = 0;
            bool safe = false;
            for (unsigned reads = 0; reads < KMS_USB_GUARD_MAX_READS; ++reads) {
                elapsed = KMS_USB_GUARD_TIME() - start;
                if (elapsed >= KMS_USB_GUARD_BUDGET_US) break;
                if (safe_phase(KMS_USB_GUARD_FRAME(), interval, margin)) { safe = true; break; }
            }
            if (elapsed > guard.max_wait_us) guard.max_wait_us = elapsed;
            // A stopped/incoherent frame clock must not hang an ISR or change
            // SDK transfer ownership. Fall through to normal activation, marked
            // as a timeout, so such a run is not evidence of full protection.
            if (!safe) ++guard.timeouts;
        }
    }
    portEXIT_CRITICAL_SAFE(&guard_lock);
}
void IRAM_ATTR kms_usb_frame_guard_after_enable(bool interrupt_in, uint32_t before, uint32_t after)
{
    if (!interrupt_in || (before & 0xffff) == (after & 0xffff)) return;
    portENTER_CRITICAL_SAFE(&guard_lock);
    ++guard.crossings;
    portEXIT_CRITICAL_SAFE(&guard_lock);
}
