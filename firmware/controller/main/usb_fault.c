#include "usb_fault.h"
#include "usb_console.h"
#include "kms_runtime.h"
#include "kms_diagnostic_store.h"
#include "kms_memory.h"
#include "usb_input.h"
#include "esp_log.h"
#include <string.h>

static kms_usb_fault_snapshot_t *latest_fault;
void kms_usb_fault_init(void)
{
    if (!latest_fault) latest_fault = kms_memory_calloc(1, sizeof(*latest_fault));
}
const kms_usb_fault_snapshot_t *kms_usb_fault_latest(void)
{ return latest_fault && latest_fault->sequence ? latest_fault : NULL; }
static void retain_field(unsigned field, unsigned value)
{
    kms_usb_input_event(KMS_EVENT_INPUT_ERROR, latest_fault->context.slot,
                          KMS_USB_FAULT_EVENT_TAG | field << 16 | (value & 65535u));
}
void kms_usb_fault_capture(const kms_usb_overdue_t *reason)
{
    kms_usb_fault_snapshot_t *snapshot = latest_fault;
    if (!snapshot) {
        ESP_LOGW("usb-fault", "Snapshot memory unavailable");
        return;
    }
    uint32_t sequence = snapshot->sequence + 1;
    memset(snapshot, 0, sizeof(*snapshot)); snapshot->sequence = sequence ? sequence : 1;
    if (reason) snapshot->reason = *reason;
    else snapshot->reason.operation = 255; // Manual or rediscovery retry.
    kms_usb_input_capture_event(&snapshot->context, KMS_EVENT_INPUT_ERROR, kms_usb_input_selected(), 0);
    kms_usb_bus_snapshot(&snapshot->bus);
    kms_usb_bus_history(&snapshot->bus_history);
    kms_usb_driver_snapshot(&snapshot->driver);
    kms_usb_health_snapshot(&snapshot->controls);
    kms_usb_devices_fault_snapshot(&snapshot->devices);
    // Capture everything before adding history or allowing root disconnect to cancel transfers.
    retain_field(KMS_USB_FAULT_OPERATION, snapshot->reason.operation << 8 | snapshot->reason.address);
    retain_field(KMS_USB_FAULT_INTERFACE, snapshot->reason.address << 8 | snapshot->reason.interface_number);
    retain_field(KMS_USB_FAULT_PENDING, snapshot->controls.pending_count);
    retain_field(KMS_USB_FAULT_PORT_LOW, snapshot->bus.hprt);
    retain_field(KMS_USB_FAULT_PORT_HIGH, snapshot->bus.hprt >> 16);
    uint32_t age = snapshot->bus.at - snapshot->bus.last_frame_change;
    retain_field(KMS_USB_FAULT_FRAME_AGE, age > 65535 ? 65535 : age);
    for (unsigned n = 0; n < snapshot->controls.pending_count; ++n) {
        const kms_usb_control_trace_t *t = &snapshot->controls.pending[n];
        if (t->address != snapshot->reason.address || t->interface_number != snapshot->reason.interface_number ||
            t->operation != snapshot->reason.operation) continue;
        retain_field(KMS_USB_FAULT_REQUEST, (unsigned)t->setup[0] << 8 | t->setup[1]);
        retain_field(KMS_USB_FAULT_VALUE, (unsigned)t->setup[3] << 8 | t->setup[2]);
        retain_field(KMS_USB_FAULT_INDEX, (unsigned)t->setup[5] << 8 | t->setup[4]);
        break;
    }
    for (unsigned n = 0; n < snapshot->devices.device_count; ++n) {
        const kms_usb_device_info_t *d = &snapshot->devices.devices[n];
        if (d->address != snapshot->reason.address) continue;
        retain_field(KMS_USB_FAULT_VID, d->vid); retain_field(KMS_USB_FAULT_PID, d->pid); break;
    }
    if (!kms_usb_console_fault_queue(snapshot)) ESP_LOGW("usb-fault", "Snapshot retained in RAM; use usb-fault when console worker is available");
}
