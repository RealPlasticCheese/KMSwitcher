#include "usb_health.h"
#include "kms_runtime.h"
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "usb/usb_host.h"

typedef struct { const void *owner; kms_usb_overdue_t transfer; kms_usb_control_trace_t trace; } watch_t;
typedef kms_usb_timing_t timing_t;
static watch_t watches[KMS_USB_CONTROL_WATCHES];
static kms_usb_control_trace_t recent[KMS_USB_RECENT_CONTROLS];
static unsigned recent_next, recent_count;
static timing_t timings[KMS_USB_OPERATIONS];
static bool fault;
static kms_usb_overdue_t failed;
static const char *const names[] = {"descriptor", "protocol", "LED", "probe"};

void kms_usb_health_init(void)
{
    memset(watches, 0, sizeof(watches)); memset(timings, 0, sizeof(timings));
    memset(recent, 0, sizeof(recent)); recent_next = recent_count = 0;
    fault = false;
}
void kms_usb_health_begin(const void *owner, unsigned operation, unsigned address, unsigned interface_number)
{
    if (!owner || operation >= KMS_USB_OPERATIONS) return;
    for (unsigned n = 0; n < sizeof(watches) / sizeof(*watches); ++n) {
        if (watches[n].owner) continue;
        uint32_t now = kms_now();
        watches[n] = (watch_t){.owner = owner,
            .transfer = {now, (uint8_t)address, (uint8_t)interface_number, (uint8_t)operation},
            .trace = {.started = now, .status = -1, .operation = (uint8_t)operation,
                .address = (uint8_t)address, .interface_number = (uint8_t)interface_number}};
        return;
    }
    ESP_LOGE("usb-health", "Transfer watch capacity exhausted");
}
static void remember(const kms_usb_control_trace_t *trace)
{
    recent[recent_next] = *trace;
    recent_next = (recent_next + 1) % KMS_USB_RECENT_CONTROLS;
    if (recent_count < KMS_USB_RECENT_CONTROLS) ++recent_count;
}
void kms_usb_health_request(const void *owner, const void *setup, int expected)
{
    for (unsigned n = 0; n < KMS_USB_CONTROL_WATCHES; ++n) if (watches[n].owner == owner && owner) {
        if (setup) memcpy(watches[n].trace.setup, setup, 8);
        watches[n].trace.expected = expected; return;
    }
}
void kms_usb_health_complete(const void *owner, int status, int actual, int expected)
{
    if (!owner) return;
    for (unsigned n = 0; n < sizeof(watches) / sizeof(*watches); ++n) {
        watch_t *w = &watches[n];
        if (w->owner != owner) continue;
        timing_t *t = &timings[w->transfer.operation];
        uint32_t elapsed = kms_now() - w->transfer.started;
        w->trace.finished = kms_now(); w->trace.status = status;
        w->trace.actual = actual; w->trace.expected = expected; remember(&w->trace);
        if (status == USB_TRANSFER_STATUS_COMPLETED && actual == expected) {
            ++t->completed;
            if (elapsed > t->max_ms) t->max_ms = elapsed;
            unsigned bin = elapsed <= 2 ? 0 : elapsed <= 8 ? 1 : elapsed <= 32 ? 2 : elapsed <= 128 ? 3 : elapsed <= 512 ? 4 : 5;
            ++t->bins[bin];
        } else ++t->failed;
        memset(w, 0, sizeof(*w)); return;
    }
}
void kms_usb_health_rejected(unsigned operation, unsigned address, unsigned interface_number, const void *setup, int error)
{
    kms_usb_control_trace_t trace = {.started = kms_now(), .finished = kms_now(), .status = error,
        .operation = (uint8_t)operation, .address = (uint8_t)address,
        .interface_number = (uint8_t)interface_number, .rejected = true};
    if (setup) memcpy(trace.setup, setup, 8);
    remember(&trace);
}
void kms_usb_health_snapshot(kms_usb_health_trace_t *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned n = 0; n < KMS_USB_CONTROL_WATCHES; ++n)
        if (watches[n].owner) out->pending[out->pending_count++] = watches[n].trace;
    out->recent_count = recent_count;
    for (unsigned n = 0; n < recent_count; ++n)
        out->recent[n] = recent[(recent_next + KMS_USB_RECENT_CONTROLS - recent_count + n) % KMS_USB_RECENT_CONTROLS];
}
bool kms_usb_health_overdue(uint32_t now, uint32_t timeout_ms, kms_usb_overdue_t *out)
{
    if (fault) { *out = failed; return true; }
    bool found = false;
    for (unsigned n = 0; n < sizeof(watches) / sizeof(*watches); ++n) {
        const watch_t *w = &watches[n];
        if (!w->owner || !kms_time_elapsed(now, w->transfer.started, timeout_ms)) continue;
        if (!found || now - w->transfer.started > now - out->started) *out = w->transfer;
        found = true;
    }
    return found;
}
void kms_usb_health_fault(unsigned operation, unsigned address, unsigned interface_number)
{
    if (fault) return;
    failed = (kms_usb_overdue_t){kms_now(), (uint8_t)address, (uint8_t)interface_number, (uint8_t)operation};
    fault = true;
}
void kms_usb_health_clear_fault(void) { fault = false; }
unsigned kms_usb_health_pending(void)
{
    unsigned count = 0;
    for (unsigned n = 0; n < sizeof(watches) / sizeof(*watches); ++n) count += watches[n].owner != NULL;
    return count;
}
void kms_usb_health_status(void)
{
    kms_usb_health_status_t snapshot;
    kms_usb_health_status_snapshot(&snapshot); kms_usb_health_print_status(&snapshot);
}
void kms_usb_health_status_snapshot(kms_usb_health_status_t *out)
{
    memcpy(out->timings, timings, sizeof(timings)); out->pending = kms_usb_health_pending();
}
void kms_usb_health_print_status(const kms_usb_health_status_t *snapshot)
{
    for (unsigned n = 0; n < KMS_USB_OPERATIONS; ++n) {
        const timing_t *t = &snapshot->timings[n];
        ESP_LOGI("usb-health", "%s completed=%" PRIu32 " failed=%" PRIu32 " max=%" PRIu32
                 "ms bins(<=2/8/32/128/512/>512ms)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32,
                 names[n], t->completed, t->failed, t->max_ms,
                 t->bins[0], t->bins[1], t->bins[2], t->bins[3], t->bins[4], t->bins[5]);
    }
    ESP_LOGI("usb-health", "Pending control transfers=%u", snapshot->pending);
}
