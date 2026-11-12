#include "usb_console.h"
#include "usb_devices.h"
#include "usb_recovery.h"
#include "usb_fault.h"
#include "usb_input.h"
#include "kms_runtime.h"
#include "kms_memory.h"
#include "usb_leds.h"
#include "usb_memory.h"
#include "usb/usb_host.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    uint32_t token, fault_sequence;
    kms_diag_record_t context;
    kms_usb_inventory_t inventory;
    kms_usb_led_status_t leds;
    kms_usb_health_status_t health;
    kms_usb_recovery_status_t recovery;
    kms_usb_memory_status_t memory;
    unsigned sources;
    bool suspended;
} status_t;
_Static_assert(sizeof(status_t) <= sizeof(kms_usb_fault_snapshot_t), "USB status must fit existing console buffers");
typedef struct {
    enum { CONSOLE_INVENTORY, CONSOLE_FAULT, CONSOLE_DRIVER, CONSOLE_STATUS } kind;
    union { kms_usb_inventory_t inventory; kms_usb_fault_snapshot_t snapshot; status_t status; } data;
} console_job_t;
static QueueHandle_t lists, available;
// Transfer ownership of two buffers instead of keeping three full snapshot
// copies. The USB owner never waits for slow UART output or overwrites a job.
static console_job_t *jobs;
static console_job_t *new_job(unsigned kind)
{
    console_job_t *job;
    if (!available || xQueueReceive(available, &job, 0) != pdTRUE) return NULL;
    job->kind = kind; return job;
}
static bool queue_job(console_job_t *job)
{
    if (xQueueSend(lists, &job, 0) == pdTRUE) return true;
    (void)xQueueSend(available, &job, 0); return false;
}
static void print_bus_state(const kms_usb_bus_snapshot_t *b, const kms_usb_bus_history_t *history)
{
    printf("KMSUSBSTATE CORE at=%" PRIu32 " ahb=%08" PRIx32 " usb=%08" PRIx32 " reset=%08" PRIx32
           " nptx=%08" PRIx32 " ptx=%08" PRIx32 " host=%08" PRIx32 " interval=%08" PRIx32
           " frame-list=%08" PRIx32 " clock=%08" PRIx32 " channel-mask=%08" PRIx32 "\n",
           b->at, b->gahbcfg, b->gusbcfg, b->grstctl, b->gnptxsts, b->hptxsts, b->hcfg,
           b->hfir, b->hflbaddr, b->pcgcctl, b->haintmsk);
    for (unsigned n = 0; n < 8; ++n)
        printf("KMSUSBSTATE DMA at=%" PRIu32 " ch=%u char=%08" PRIx32 " size=%08" PRIx32
               " descriptor=%08" PRIx32 " buffer=%08" PRIx32 "\n", b->at, n,
               b->channels[n].characteristics, b->channels[n].size, b->channels[n].dma, b->channels[n].buffer);
    for (unsigned n = 0; n < history->count; ++n) {
        const kms_usb_bus_sample_t *s = &history->samples[n];
        printf("KMSUSBSTATE SAMPLE at=%" PRIu32 " age=%" PRIu32 " frame=%08" PRIx32 " ahb=%08" PRIx32
               " reset=%08" PRIx32 " nptx=%08" PRIx32 " ptx=%08" PRIx32 " host=%08" PRIx32
               " clock=%08" PRIx32 " core=%08" PRIx32 " mask=%08" PRIx32 " channels=%08" PRIx32
               " irqs=%" PRIu32 " last-irq=%" PRIu32 "\n", s->at, b->at - s->at, s->hfnum, s->gahbcfg,
               s->grstctl, s->gnptxsts, s->hptxsts, s->hcfg, s->pcgcctl, s->gintsts, s->gintmsk,
               s->haint, s->interrupts, s->last_interrupt);
        for (unsigned c = 0; c < 8; ++c)
            printf("KMSUSBSTATE PROGRESS at=%" PRIu32 " ch=%u char=%08" PRIx32 " size=%08" PRIx32
                   " descriptor=%08" PRIx32 " buffer=%08" PRIx32 "\n", s->at, c,
                   s->channels[c].characteristics, s->channels[c].size, s->channels[c].dma, s->channels[c].buffer);
    }
}
static void print_driver(const kms_usb_driver_snapshot_t *s)
{
    printf("KMSUSBTRACE BEGIN events=%u total=%" PRIu32 " root-irqs=%" PRIu32 " last-irq=%" PRIu32
           " root-events=%" PRIu32 " last-root=%" PRIu32 " core=%08" PRIx32 " port=%08" PRIx32 "\n",
           s->event_count, s->total_events, s->root_interrupts, s->last_root_interrupt,
           s->root_events, s->last_root_event, s->root_bits, s->port_bits);
    const kms_usb_frame_guard_stats_t *g = &s->frame_guard;
    printf("KMSUSBTRACE WORKAROUND enabled=%" PRIu32 " guard-us=%u budget-us=%u activations=%" PRIu32
           " waited=%" PRIu32 " timeouts=%" PRIu32 " max-wait-us=%" PRIu32 " crossings=%" PRIu32 "\n",
           g->enabled, KMS_USB_GUARD_US, KMS_USB_GUARD_BUDGET_US, g->activations,
           g->waited, g->timeouts, g->max_wait_us, g->crossings);
    for (unsigned n = 0; n < KMS_USB_DRIVER_CHANNELS; ++n) {
        const kms_usb_driver_channel_t *c = &s->channels[n];
        printf("KMSUSBTRACE CHANNEL %u starts=%" PRIu32 " irqs=%" PRIu32 " errors=%" PRIu32
               " start=%" PRIu32 " irq=%" PRIu32 " error=%" PRIu32 " char=%08" PRIx32
               " bits=%08" PRIx32 " error-bits=%08" PRIx32 " error-char=%08" PRIx32 " flags=%08" PRIx32 "\n",
               n, c->starts, c->interrupts, c->errors, c->last_start, c->last_interrupt, c->last_error,
               c->characteristics, c->interrupt, c->error_bits, c->error_characteristics, c->flags);
        if (c->descriptor_count)
            printf("KMSUSBTRACE QTD ch=%u base=%08" PRIx32 " count=%" PRIu32 " start=%" PRIu32
                   " submitted=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
                   " current=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n", n,
                   c->descriptor_base, c->descriptor_count, c->descriptor_start,
                   c->submitted_status[0], c->submitted_status[1], c->submitted_status[2],
                   c->current_status[0], c->current_status[1], c->current_status[2]);
    }
    for (unsigned n = 0; n < s->event_count; ++n) {
        const kms_usb_driver_event_t *e = &s->events[n];
        printf("KMSUSBTRACE EVENT at=%" PRIu32 " kind=%u ch=%u detail=%u char=%08" PRIx32
               " bits=%08" PRIx32 " size=%08" PRIx32 " flags=%08" PRIx32 " extra=%08" PRIx32 "\n",
               e->at, e->kind, e->channel, e->detail, e->characteristics, e->interrupt, e->size, e->flags, e->extra);
    }
    printf("KMSUSBTRACE END\n");
}
static void print_control(const char *kind, const kms_usb_control_trace_t *t, uint32_t at)
{
    printf("KMSUSBFAULT %s %u/%u op=%u start=%" PRIu32 " age=%" PRIu32 " end=%" PRIu32
           " status=%d rejected=%u bytes=%d/%d setup=%02x%02x%02x%02x%02x%02x%02x%02x\n", kind, t->address, t->interface_number,
           t->operation, t->started, at - t->started, t->finished, t->status, t->rejected, t->actual, t->expected,
           t->setup[0], t->setup[1], t->setup[2], t->setup[3], t->setup[4], t->setup[5], t->setup[6], t->setup[7]);
}
static void print_stream(const kms_usb_stream_status_t *s)
{
    printf("KMSUSBSTREAM COUNTS completed=%" PRIu32 " pending=%u peak=%u/%u dropped=%" PRIu32
           " stale=%" PRIu32 " expired=%" PRIu32 " overflows=%" PRIu32 " last-complete=%" PRIu32 "\n",
           s->completed, s->pending, s->peak, KMS_USB_RAW_QUEUE_DEPTH, s->dropped, s->stale,
           s->expired, s->overflows, s->last_complete);
    printf("KMSUSBSTREAM TIMING callback-us=%" PRIu32 " callback-at=%" PRIu32
           " dispatch-us=%" PRIu32 " dispatch-at=%" PRIu32 " poll-gap-us=%" PRIu32 " gap-at=%" PRIu32
           " decode-us=%" PRIu32 " decode-at=%" PRIu32 "\n", s->callback.max_us, s->callback.at,
           s->dispatch.max_us, s->dispatch.at, s->poll_gap.max_us, s->poll_gap.at, s->decode.max_us, s->decode.at);
    for (unsigned n = 0; n < s->recent_count && n < KMS_USB_STREAM_DELAYS; ++n)
        printf("KMSUSBSTREAM DELAY at=%" PRIu32 " gap-us=%" PRIu32 " dispatch-us=%" PRIu32 "\n",
               s->recent[n].at, s->recent[n].gap_us, s->recent[n].dispatch_us);
}
static void print_fault(const kms_usb_fault_snapshot_t *s)
{
    printf("KMSUSBFAULT BEGIN %" PRIu32 " boot=%08" PRIx32 " at=%" PRIu32 " slot=%u op=%u target=%u/%u\n",
           s->sequence, s->context.boot, s->context.uptime_ms, s->context.slot,
           s->reason.operation, s->reason.address, s->reason.interface_number);
    print_stream(&s->devices.stream);
    const kms_usb_bus_snapshot_t *b = &s->bus;
    printf("KMSUSBFAULT BUS at=%" PRIu32 " frame-age=%" PRIu32 " hfnum=%08" PRIx32 " hprt=%08" PRIx32
           " gintsts=%08" PRIx32 " gintmsk=%08" PRIx32 " haint=%08" PRIx32 "\n",
           b->at, b->at - b->last_frame_change, b->hfnum, b->hprt, b->gintsts, b->gintmsk, b->haint);
    for (unsigned n = 0; n < 8; ++n)
        printf("KMSUSBFAULT CHANNEL %u char=%08" PRIx32 " irq=%08" PRIx32 " mask=%08" PRIx32 " size=%08" PRIx32 "\n",
               n, b->channels[n].characteristics, b->channels[n].interrupt, b->channels[n].mask, b->channels[n].size);
    print_bus_state(b, &s->bus_history);
    for (unsigned n = 0; n < s->devices.device_count; ++n) {
        const kms_usb_device_info_t *d = &s->devices.devices[n];
        printf("KMSUSBFAULT DEVICE %u %04x:%04x parent=%u port=%u speed=%u\n", d->address, d->vid, d->pid, d->parent, d->port, d->speed);
    }
    for (unsigned n = 0; n < s->devices.interface_count; ++n) {
        const kms_usb_interface_info_t *i = &s->devices.interfaces[n];
        printf("KMSUSBFAULT INPUT %u/%u ep=%02x state=%u pending=%u seen=%u submit=%" PRIu32
               " complete=%" PRIu32 " status=%d bytes=%d reports=%" PRIu32 "\n", i->address, i->number, i->endpoint,
               i->state, i->transfer_pending, i->completed_once, i->last_submit, i->last_complete, i->last_status, i->last_bytes, i->reports);
    }
    for (unsigned n = 0; n < s->controls.pending_count; ++n) print_control("PENDING", &s->controls.pending[n], b->at);
    for (unsigned n = 0; n < s->controls.recent_count; ++n) print_control("RECENT", &s->controls.recent[n], b->at);
    print_driver(&s->driver);
    printf("KMSUSBFAULT END\n");
}
bool kms_usb_console_fault_queue(const struct kms_usb_fault_snapshot *snapshot)
{
    if (!lists || !snapshot || !snapshot->sequence) return false;
    console_job_t *job = new_job(CONSOLE_FAULT);
    if (!job) return false;
    job->data.snapshot = *snapshot; return queue_job(job);
}
static void print_status(const status_t *s)
{
    if (s->token) {
        printf("KMSUSBSTATUS BEGIN id=%" PRIu32 " boot=%08" PRIx32 " at=%" PRIu32 " slot=%u fault=%" PRIu32 "\n",
               s->token, s->context.boot, s->context.uptime_ms, s->context.slot, s->fault_sequence);
        printf("KMSUSBSTATUS STATE state=%u attempts=%u recovered=%u restarts=%" PRIu32
               " controls=%u hubs=%u inputs=%u capacity=%u sources=%u suspended=%u pending=%u\n",
               s->recovery.state, s->recovery.attempts, s->recovery.recoveries, s->recovery.restarts,
               s->inventory.controls, s->inventory.hubs, s->inventory.inputs, KMS_USB_CHANNELS,
               s->sources, s->suspended, s->health.pending);
    }
    kms_usb_leds_print_status(&s->leds); kms_usb_health_print_status(&s->health);
    kms_usb_recovery_print_status(&s->recovery);
    print_stream(&s->inventory.stream);
    unsigned active = 0;
    for (unsigned n = 0; n < s->inventory.interface_count; ++n) active += s->inventory.interfaces[n].state == KMS_USB_ACTIVE;
    ESP_LOGI("usb-input", "USB devices=%u, HID interfaces=%u, input sources=%u, root=%s",
             s->inventory.controls, active, s->sources, s->suspended ? "suspended" : "running");
    ESP_LOGI("usb-input", "Channels: %u/%u (%u control + %u hub status + %u claimed input); usb-list shows choices",
             s->inventory.controls + s->inventory.hubs + s->inventory.inputs, KMS_USB_CHANNELS,
             s->inventory.controls, s->inventory.hubs, s->inventory.inputs);
    for (unsigned n = 0; n < s->inventory.interface_count; ++n) {
        const kms_usb_interface_info_t *i = &s->inventory.interfaces[n];
        if (i->device_class == 3) ESP_LOGI("usb-input", "Address %u interface %u: %s, reports=%" PRIu32 ", decoded=%" PRIu32,
            i->address, i->number, kms_usb_interface_state(i->state), i->reports, i->decoded);
    }
    kms_usb_memory_print(&s->memory);
    if (s->token) printf("KMSUSBSTATUS END id=%" PRIu32 "\n", s->token);
}
static void print_job(const console_job_t *job)
{
    if (job->kind == CONSOLE_FAULT) { print_fault(&job->data.snapshot); return; }
    if (job->kind == CONSOLE_STATUS) { print_status(&job->data.status); return; }
    if (job->kind == CONSOLE_DRIVER) {
        print_bus_state(&job->data.snapshot.bus, &job->data.snapshot.bus_history);
        print_driver(&job->data.snapshot.driver); return;
    }
    const kms_usb_inventory_t *s = &job->data.inventory;
    printf("KMSUSB BEGIN %u %u %u %u %u\n", s->automatic, s->controls, s->hubs, s->inputs, KMS_USB_CHANNELS);
    for (unsigned n = 0; n < s->device_count; ++n) {
        const kms_usb_device_info_t *d = &s->devices[n];
        char name[sizeof(d->name) * 2 + 1];
        unsigned j;
        for (j = 0; d->name[j] && j < sizeof(d->name) - 1; ++j)
            snprintf(name + j * 2, 3, "%02x", (unsigned char)d->name[j]);
        name[j * 2] = 0;
        printf("KMSUSB DEVICE %u %04x %04x %08" PRIx32 " %u %u %u %s\n",
               d->address, d->vid, d->pid, d->serial, d->parent, d->port, d->speed, j ? name : "-");
    }
    for (unsigned n = 0; n < s->interface_count; ++n) {
        const kms_usb_interface_info_t *i = &s->interfaces[n];
        printf("KMSUSB INTERFACE %u %u %02x %u %u %u %s %u %" PRIu32 " %" PRIu32 "\n",
               i->address, i->number, i->device_class, i->endpoints, i->kinds, i->wanted,
               kms_usb_interface_state(i->state), i->descriptor_bytes, i->reports, i->decoded);
    }
    printf("KMSUSB END\n");
}
static void list_worker(void *arg)
{
    (void)arg; console_job_t *job;
    while (true) {
        if (xQueueReceive(lists, &job, portMAX_DELAY) != pdTRUE) continue;
        print_job(job); (void)xQueueSend(available, &job, 0);
    }
}
void kms_usb_console_init(void)
{
    jobs = kms_memory_calloc(2, sizeof(*jobs));
    if (!jobs) {
        ESP_LOGW("usb-input", "USB console snapshot memory unavailable; input continues");
        return;
    }
    lists = xQueueCreate(2, sizeof(console_job_t *));
    available = xQueueCreate(2, sizeof(console_job_t *));
    if (available) for (unsigned n = 0; n < 2; ++n) {
        console_job_t *job = &jobs[n]; (void)xQueueSend(available, &job, 0);
    }
    if (!lists || !available || xTaskCreate(list_worker, "usb-list", 3072, NULL, 1, NULL) != pdPASS) {
        if (lists) vQueueDelete(lists);
        if (available) vQueueDelete(available);
        lists = available = NULL;
        free(jobs); jobs = NULL;
    }
}
void kms_usb_console_list(void)
{
    if (!lists) { ESP_LOGW("usb-input", "USB inventory worker unavailable"); return; }
    console_job_t *job = new_job(CONSOLE_INVENTORY);
    if (!job) { ESP_LOGW("usb-input", "USB inventory busy; retry shortly"); return; }
    kms_usb_devices_snapshot(&job->data.inventory);
    if (!queue_job(job)) ESP_LOGW("usb-input", "USB inventory busy; retry shortly");
}
bool kms_usb_console_command(const char *line)
{
    if (strncmp(line, "usb-", 4)) return false;
    if (!kms_usb_input_command(line)) printf("KMSUSB RESULT busy\n");
    return true;
}
bool kms_usb_console_execute(const char *line)
{
    uint32_t token = 0;
    bool framed = !strncmp(line, "usb-status ", 11);
    if (framed) {
        const char *p = line + 11;
        for (; *p >= '0' && *p <= '9'; ++p) {
            unsigned digit = (unsigned)(*p - '0');
            if (token > (UINT32_MAX - digit) / 10) break;
            token = token * 10 + digit;
        }
        if (*p || !token) { printf("KMSUSB RESULT error\n"); return true; }
    }
    if (!strcmp(line, "usb-status") || framed) {
        console_job_t *job = new_job(CONSOLE_STATUS);
        if (!job) { ESP_LOGW("usb-input", "USB status busy; retry shortly"); return true; }
        status_t *s = &job->data.status;
        s->token = token;
        if (token) {
            kms_usb_input_capture_event(&s->context, KMS_EVENT_INPUT_ERROR, kms_usb_input_selected(), 0);
            s->context.uptime_ms = kms_now();
            const kms_usb_fault_snapshot_t *f = kms_usb_fault_latest(); s->fault_sequence = f ? f->sequence : 0;
        }
        kms_usb_devices_snapshot(&s->inventory); kms_usb_leds_snapshot(&s->leds);
        kms_usb_health_status_snapshot(&s->health); kms_usb_recovery_status_snapshot(&s->recovery);
        kms_usb_memory_snapshot(&s->memory);
        s->sources = kms_usb_input_sources();
        usb_host_lib_info_t host;
        s->suspended = usb_host_lib_info(&host) == ESP_OK && host.root_port_suspended;
        (void)queue_job(job); return true;
    }
    if (!strcmp(line, "usb-trace")) {
        console_job_t *job = new_job(CONSOLE_DRIVER);
        if (!job) { printf("KMSUSBTRACE BUSY\n"); return true; }
        kms_usb_bus_snapshot(&job->data.snapshot.bus);
        kms_usb_bus_history(&job->data.snapshot.bus_history);
        kms_usb_driver_snapshot(&job->data.snapshot.driver);
        if (!queue_job(job)) printf("KMSUSBTRACE BUSY\n");
        return true;
    }
    if (!strcmp(line, "usb-fault")) {
        const kms_usb_fault_snapshot_t *snapshot = kms_usb_fault_latest();
        if (!snapshot) printf("KMSUSBFAULT NONE\n");
        else if (!kms_usb_console_fault_queue(snapshot)) printf("KMSUSBFAULT BUSY\n");
        return true;
    }
#ifdef CONFIG_KMS_TEST_SUPPORT
    if (!strcmp(line, "usb-test-block")) {
        printf("KMSUSB RESULT %s\n", kms_usb_recovery_test_block() ? "queued" : "busy"); return true;
    }
#endif
    if (!strcmp(line, "usb-list")) { kms_usb_console_list(); return true; }
    if (!strcmp(line, "usb-rescan")) { kms_usb_devices_rescan(); printf("KMSUSB RESULT ok\n"); return true; }
    if (!strcmp(line, "usb-recover")) {
        printf("KMSUSB RESULT %s\n", kms_usb_recovery_request() ? "queued" : "busy"); return true;
    }
    unsigned address, number; char setting[4], extra;
    if (sscanf(line, "usb-input %u %u %3s %c", &address, &number, setting, &extra) == 3 &&
        (!strcmp(setting, "on") || !strcmp(setting, "off"))) {
        printf("KMSUSB RESULT %s\n", kms_usb_devices_select(address, number, !strcmp(setting, "on")) ? "ok" : "error");
        return true;
    }
    if (sscanf(line, "usb-auto %3s %c", setting, &extra) == 1 &&
        (!strcmp(setting, "on") || !strcmp(setting, "off"))) {
        printf("KMSUSB RESULT %s\n", kms_usb_devices_automatic(!strcmp(setting, "on")) ? "ok" : "error");
        return true;
    }
    if (!strncmp(line, "usb-", 4)) {
        printf("KMSUSB RESULT error\n");
        ESP_LOGW("usb-input", "Use usb-list, usb-fault, usb-trace, usb-rescan, usb-recover, usb-input ADDRESS INTERFACE on|off, usb-auto on|off");
        return true;
    }
    return false;
}
