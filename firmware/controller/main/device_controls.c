#include "device_controls.h"
#include "preferences.h"
#include "usb_devices.h"
#include "usb_input.h"
#include "kms_remote.h"
#include "kms_radio_channel.h"
#include "kms_channel_recovery.h"
#include "kms_test_mode.h"
#include "kms_memory.h"
#include <string.h>

enum device_action { DEVICE_STATUS, DEVICE_PAIR, DEVICE_UNPAIR, DEVICE_PAUSE, DEVICE_RESUME,
                     DEVICE_JIGGLER_GET, DEVICE_JIGGLER_SET, DEVICE_JIGGLER_MODE };
enum usb_action { USB_AUTOMATIC, USB_ALLOW, USB_RETRY };

static unsigned unpair_slot;
static uint32_t unpair_at;
static kms_usb_inventory_t *usb_snapshot;
static unsigned usb_owner;
static uint32_t usb_boot, usb_session, usb_token, usb_at;

static unsigned result(const char *error)
{
    if (!error) return KMS_D_OK;
    if (!strcmp(error, "BUSY")) return KMS_D_BUSY;
    if (!strcmp(error, "STORAGE")) return KMS_D_STORAGE;
    if (!strcmp(error, "UNSUPPORTED") || !strcmp(error, "UPGRADE_RECEIVER")) return KMS_D_UNSUPPORTED;
    if (!strcmp(error, "OFFLINE")) return KMS_D_OFFLINE;
    return KMS_D_INVALID;
}
static bool device_request(unsigned source, uint32_t boot, kms_desktop_message_t *m)
{
    if (m->op >= KMS_D_USB_SNAPSHOT && m->op <= KMS_D_USB_CHANGE) {
        if (m->op == KMS_D_USB_CHANGE && (kms_remote_transfer_busy() || kms_test_mode_active())) {
            m->result = KMS_D_BUSY; return true;
        }
        if (!kms_usb_input_desktop(source, boot, m)) { m->result = KMS_D_BUSY; return true; }
        return false;
    }
    if (m->op == KMS_D_REMOTE_REQUEST || m->op == KMS_D_REMOTE_READ) {
        kms_remote_desktop(source, boot, m); return true;
    }
    if (m->op == KMS_D_PREFERENCES_DEFAULT) {
        if (kms_remote_transfer_busy()) m->result = KMS_D_BUSY;
        else if (kms_get_u32(m->data) != kms_preferences_revision()) m->result = KMS_D_CONFLICT;
        else {
            kms_user_preferences_t p; kms_user_preferences_defaults(&p);
            if (!kms_preferences_set(&p)) m->result = KMS_D_STORAGE;
        }
        kms_put_u32(m->data, kms_preferences_revision());
        (void)kms_user_preferences_encode(kms_preferences_get(), m->data + 4);
        return true;
    }
    if (m->op == KMS_D_CHANNEL_RECOVERY_GET || m->op == KMS_D_CHANNEL_RECOVERY_SET) {
        kms_channel_recovery_command(m); return true;
    }
    if (m->op != KMS_D_DEVICE_CONTROL) { m->result = KMS_D_UNSUPPORTED; return true; }
    unsigned action = kms_get_u32(m->data), slot = kms_get_u32(m->data + 4), value = kms_get_u32(m->data + 8);
    if (action > DEVICE_JIGGLER_MODE || ((action == DEVICE_PAIR || action == DEVICE_UNPAIR || action >= DEVICE_JIGGLER_GET) &&
        (!slot || slot > KMS_SLOTS))) {
        m->result = KMS_D_INVALID; return true;
    }
    if (action != DEVICE_STATUS && action != DEVICE_JIGGLER_GET &&
        (kms_link_pairing() || kms_remote_transfer_busy() ||
        (kms_radio_channel_changing() && !(action == DEVICE_PAUSE && kms_radio_channel_trial_active())) ||
        kms_test_mode_active() || unpair_slot)) { m->result = KMS_D_BUSY; return true; }
    if (action == DEVICE_PAIR) { kms_link_pair(NULL, slot); if (!kms_link_pairing()) m->result = KMS_D_BUSY; }
    else if (action == DEVICE_UNPAIR) {
        // Leave the initiating radio peer alive long enough for its ACK.
        if (slot == source) { unpair_slot = slot; unpair_at = kms_now(); }
        else m->result = (uint8_t)result(kms_link_unpair(slot));
    } else if (action == DEVICE_PAUSE) { kms_link_pause(); kms_usb_input_block(true); }
    else if (action == DEVICE_RESUME) { kms_link_resume(); if (!kms_link_selected()) m->result = KMS_D_OFFLINE; }
    if (action >= DEVICE_JIGGLER_GET) {
        if ((action == DEVICE_JIGGLER_SET && value > 1) || (action == DEVICE_JIGGLER_MODE && value >= KMS_JIGGLER_MODE_COUNT)) {
            m->result = KMS_D_INVALID; return true;
        }
        const char *error = action == DEVICE_JIGGLER_SET ? kms_link_jiggler_set(slot, value != 0) :
            action == DEVICE_JIGGLER_MODE ? kms_link_jiggler_set_mode(slot, value) : NULL;
        kms_jiggler_status_t s = {0};
        if (!error) error = kms_link_jiggler_get(slot, &s);
        m->result = (uint8_t)result(error); memset(m->data, 0, sizeof(m->data));
        uint32_t values[] = {s.boot, s.sequence, s.seconds, s.distance, s.mode, s.enabled, s.pending, s.modes_supported};
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) kms_put_u32(m->data + i * 4, values[i]);
    } else {
        memset(m->data, 0, sizeof(m->data)); unsigned paired = 0;
        for (unsigned i = 1; i <= KMS_SLOTS; ++i) if (kms_link_has_pair(i)) paired |= 1u << (i - 1);
        kms_put_u32(m->data, paired); kms_put_u32(m->data + 4, kms_link_selected());
        kms_put_u32(m->data + 8, kms_link_pairing());
    }
    return true;
}
void kms_device_controls_init(void) { kms_desktop_device_handler(device_request); }
void kms_device_controls_tick(void)
{
    kms_usb_input_desktop_replies();
    if (unpair_slot && kms_time_elapsed(kms_now(), unpair_at, 500)) {
        (void)kms_link_unpair(unpair_slot); unpair_slot = 0;
    }
}
void kms_device_usb_request(kms_desktop_message_t *m, unsigned source, uint32_t boot)
{
    uint32_t token = kms_get_u32(m->data), index = kms_get_u32(m->data + 4);
    if (m->op == KMS_D_USB_SNAPSHOT) {
        if (!usb_snapshot) usb_snapshot = kms_memory_calloc(1, sizeof(*usb_snapshot));
        if (!usb_snapshot) { m->result = KMS_D_UNSUPPORTED; return; }
        kms_usb_devices_snapshot(usb_snapshot); usb_owner = source; usb_boot = boot;
        usb_session = m->session; usb_token = m->id; usb_at = kms_now();
        memset(m->data, 0, sizeof(m->data));
        uint32_t values[] = {usb_token, usb_snapshot->device_count, usb_snapshot->interface_count,
            usb_snapshot->controls, usb_snapshot->hubs, usb_snapshot->inputs, usb_snapshot->automatic, KMS_USB_CHANNELS};
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) kms_put_u32(m->data + i * 4, values[i]);
        return;
    }
    if (!usb_snapshot || usb_owner != source || usb_boot != boot || usb_session != m->session || usb_token != token ||
        kms_time_elapsed(kms_now(), usb_at, 120000)) { m->result = KMS_D_STALE; return; }
    if (m->op == KMS_D_USB_CHANGE) {
        unsigned action = index, address = kms_get_u32(m->data + 8), number = kms_get_u32(m->data + 12);
        unsigned enabled = kms_get_u32(m->data + 16);
        if (enabled > 1 || action > USB_RETRY) { m->result = KMS_D_INVALID; return; }
        if (action == USB_AUTOMATIC) { if (!kms_usb_devices_automatic(enabled != 0)) m->result = KMS_D_STORAGE; }
        else if (action == USB_ALLOW) {
            const kms_usb_device_info_t *device = NULL;
            for (unsigned i = 0; i < usb_snapshot->device_count; ++i)
                if (usb_snapshot->devices[i].address == address) device = &usb_snapshot->devices[i];
            if (!device || !kms_usb_devices_matches(device)) m->result = KMS_D_STALE;
            else if (!kms_usb_devices_select(address, number, enabled != 0)) m->result = KMS_D_INVALID;
        } else kms_usb_devices_rescan();
        return;
    }
    memset(m->data, 0, sizeof(m->data));
    if (m->op == KMS_D_USB_DEVICE && index < usb_snapshot->device_count) {
        const kms_usb_device_info_t *d = &usb_snapshot->devices[index];
        kms_put_u32(m->data, token); kms_put_u32(m->data + 4, index);
        kms_put_u32(m->data + 8, d->vid | (uint32_t)d->pid << 16); kms_put_u32(m->data + 12, d->serial);
        m->data[16] = d->address; m->data[17] = d->parent; m->data[18] = d->port; m->data[19] = d->speed;
        memcpy(m->data + 20, d->name, 27);
    } else if (m->op == KMS_D_USB_INTERFACE && index < usb_snapshot->interface_count) {
        const kms_usb_interface_info_t *i = &usb_snapshot->interfaces[index];
        uint32_t values[] = {token, index, i->address, i->number, i->device_class, i->endpoints,
            i->kinds, i->wanted, i->state, i->descriptor_bytes, i->reports, i->decoded};
        for (unsigned n = 0; n < sizeof(values) / sizeof(values[0]); ++n) kms_put_u32(m->data + n * 4, values[n]);
    } else m->result = KMS_D_INVALID;
}
