// Production lifecycle matrix. Reuse the asynchronous adapter, not its verdicts.
#include "test_usb_host.c"

enum { PRESSURE_NONE, PRESSURE_CLAIM_FIRST, PRESSURE_CLAIM_SECOND,
       PRESSURE_PARSER, PRESSURE_CONTROL, PRESSURE_INPUT, PRESSURE_STAGES };
static unsigned matrix_cases, matrix_case;
static const uint8_t vendor_report[] = {6,0,255,9,1,0xa1,1,0x15,0,0x26,255,0,0x75,8,0x95,1,0x81,2,0xc0};

static void receiver(unsigned address, bool k830, unsigned mps)
{
    announce(address, 1, 3, k830 ? 3 : 2);
    fake_device_t *d = &fake_devices[address];
    d->desc.idVendor = 0x046d; d->desc.idProduct = k830 ? 0xc52b : 0xc534;
    unsigned number = 0;
    for (unsigned at = sizeof(usb_config_desc_t); at < ((usb_config_desc_t *)d->config)->wTotalLength; at += d->config[at]) {
        if (d->config[at + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) number = d->config[at + 2];
        if (d->config[at + 1] == HID_DESCRIPTOR && number == 2) {
            d->reports[2] = vendor_report; d->report_bytes[2] = sizeof(vendor_report);
            d->config[at + 7] = sizeof(vendor_report); d->config[at + 8] = 0;
        }
        if (d->config[at + 1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT)
            ((usb_ep_desc_t *)(d->config + at))->wMaxPacketSize = (uint16_t)mps;
    }
}
static unsigned live_parsers(void)
{ unsigned count = 0; for (unsigned n = 0; n < 24; ++n) count += allocations[n].used; return count; }
static unsigned live_transfers(void)
{ unsigned count = 0; for (unsigned n = 0; n < 32; ++n) count += fake_transfers[n].used; return count; }
static void pressure(unsigned stage, unsigned address, unsigned failures)
{
    fail_address = address; fail_interface = stage == PRESSURE_CLAIM_SECOND ? 1 : 0;
    claim_failures = parser_failures = control_alloc_failures = input_alloc_failures = 0;
    if (stage == PRESSURE_CLAIM_FIRST || stage == PRESSURE_CLAIM_SECOND) claim_failures = failures;
    if (stage == PRESSURE_PARSER) parser_failures = failures;
    if (stage == PRESSURE_CONTROL) control_alloc_failures = failures;
    if (stage == PRESSURE_INPUT) input_alloc_failures = failures;
}
static unsigned remaining_pressure(void)
{ return claim_failures + parser_failures + control_alloc_failures + input_alloc_failures; }
static device_t *owner(unsigned address)
{
    for (unsigned n = 0; n < KMS_USB_DEVICES; ++n)
        if (devices[n].handle == &fake_devices[address]) return &devices[n];
    return NULL;
}
static bool active_pair(unsigned address)
{
    kms_usb_inventory_t s; kms_usb_devices_snapshot(&s);
    const kms_usb_interface_info_t *a = row(&s, address, 0), *b = row(&s, address, 1);
    return a && b && a->state == KMS_USB_ACTIVE && b->state == KMS_USB_ACTIVE;
}
static int sibling_progress(bool present)
{
    if (!present) return 0;
    unsigned before = received;
    CHECK(active_pair(3));
    CHECK(dispatch_report(3, 0, 4, 1));
    CHECK(dispatch_report(3, 1, 1, 2));
    kms_usb_devices_tick();
    CHECK(received == before + 2 && !callback_side_effects && !inline_reports);
    return 0;
}
static int clean_all(unsigned target, bool other)
{
    unplug(target); if (other) unplug(3); unplug(1); pump(20);
    CHECK(!kms_usb_devices_active() && !live_parsers() && !live_transfers());
    CHECK(claims == releases && mistakes == 0);
    return 0;
}
static int lifecycle_case(unsigned type, unsigned other, unsigned stage, unsigned duration,
                          unsigned ordering, unsigned wrap, unsigned mps)
{
    ++matrix_cases;
    matrix_case = type | other << 1 | stage << 2 | duration << 5 | ordering << 7 | wrap << 9 | mps << 10;
    reset(true); if (wrap) fake_now = UINT32_MAX - 200;
    announce(1, 0, 9, 1); receiver(2, type == 0, mps);
    if (other) receiver(3, type != 0, mps);
    pump(30); CHECK(active_pair(2) && (!other || active_pair(3)));
    unsigned initial_parsers = live_parsers(), initial_transfers = live_transfers();
    unsigned target = ordering == 2 ? 2 : 4;
    // Address reuse is legal only after the SDK has destroyed the old object.
    hold_cancellations = ordering == 1; unplug(2);
    if (ordering != 1) pump(12);
    pressure(stage, target, duration == 0 ? 1 : duration == 1 ? 3 : 1000);
    receiver(target, type == 0, mps); pump(20);
    if (ordering == 1) {
        CHECK(early_claims == 0 && fake_devices[2].opened);
        hold_cancellations = false; pump(30);
    }
    if (stage != PRESSURE_NONE) {
        CHECK(owner(target) && owner(target)->retrying && !active_pair(target));
        unsigned attempts = claim_attempts, left = remaining_pressure();
        pump(100);
        CHECK(attempts == claim_attempts && left == remaining_pressure());
        CHECK(live_parsers() == (other ? 2u : 0u));
        // The last complete working receiver continues through every retry.
        for (unsigned n = 0; n < 5; ++n) {
            int result = sibling_progress(other != 0); if (result) return result;
            fake_now += KMS_USB_ALLOC_RETRY_MS; pump(30);
            CHECK(mistakes == 0 && early_claims == 0);
            CHECK(live_parsers() <= initial_parsers && live_transfers() <= initial_transfers);
        }
        if (duration == 2) {
            CHECK(!active_pair(target) && remaining_pressure() > 0);
            pressure(PRESSURE_NONE, target, 0);
            fake_now += KMS_USB_ALLOC_RETRY_MS; pump(30);
        }
    }
    CHECK(active_pair(target) && kms_usb_devices_active() == 2 + 2 * other);
    CHECK(live_parsers() == initial_parsers && live_transfers() == initial_transfers);
    kms_usb_inventory_t s; kms_usb_devices_snapshot(&s);
    CHECK(s.controls + s.hubs + s.inputs <= 8 && s.stream.dropped == 0 && s.stream.overflows == 0);
    if (type == 0) CHECK(row(&s, target, 2)->state == KMS_USB_UNSUPPORTED);
    int result = sibling_progress(other != 0); if (result) return result;
    return clean_all(target, other != 0);
}

static int intervention_case(unsigned stage, unsigned action, unsigned wrap)
{
    ++matrix_cases; matrix_case = 0x80000000u | stage | action << 4 | wrap << 8;
    reset(true); if (wrap) fake_now = UINT32_MAX - 100;
    announce(1, 0, 9, 1); receiver(2, false, 64); receiver(3, true, 8); pump(30);
    unplug(2); pump(12); pressure(stage, 4, 1000); receiver(4, false, 64); pump(30);
    CHECK(owner(4) && owner(4)->retrying && active_pair(3));
    if (action == 0) {
        // Unplug during repeated allocation failure cancels the episode.
        unplug(4); pump(12); unsigned before = claim_attempts;
        fake_now += 2 * KMS_USB_ALLOC_RETRY_MS; pump(20);
        CHECK(!owner(4) && claim_attempts == before && active_pair(3));
        unplug(3); unplug(1); pump(20);
    } else if (action == 1) {
        // A user disabling this receiver must prevent fresh claims; enabling
        // it again after pressure clears restores the entire bundle.
        CHECK(kms_usb_devices_select(4, 0, false));
        unsigned before = claim_attempts; fake_now += KMS_USB_ALLOC_RETRY_MS; pump(20);
        CHECK(claim_attempts == before);
        CHECK(active_pair(3));
        pressure(PRESSURE_NONE, 4, 0); CHECK(kms_usb_devices_select(4, 0, true));
        fake_now += KMS_USB_ALLOC_RETRY_MS; pump(30); CHECK(active_pair(4));
        int result = clean_all(4, true); if (result) return result;
    } else if (action == 2) {
        // Explicit rescan also retries unsupported descriptors. Global parser
        // pressure may therefore affect both devices until memory is available.
        kms_usb_devices_rescan(); pump(20); CHECK(!active_pair(4));
        pressure(PRESSURE_NONE, 4, 0); fake_now += KMS_USB_ALLOC_RETRY_MS; pump(30);
        CHECK(active_pair(4) && active_pair(3)); int result = clean_all(4, true); if (result) return result;
    } else {
        // Model the observed controlled root reconnect: stop software owners,
        // receive disconnect completions, then rediscover both devices.
        hold_cancellations = true; kms_usb_devices_quiesce(true);
        unplug(4); unplug(3); unplug(1); pump(20);
        CHECK(!kms_usb_devices_drained());
        hold_cancellations = false; pump(20);
        CHECK(kms_usb_devices_drained() && !live_parsers() && !live_transfers());
        pressure(PRESSURE_NONE, 4, 0); kms_usb_devices_quiesce(false);
        announce(1, 0, 9, 1); receiver(4, false, 64); receiver(3, true, 8); pump(30);
        CHECK(active_pair(4) && active_pair(3));
        int result = clean_all(4, true); if (result) return result;
    }
    CHECK(!live_parsers() && !live_transfers() && claims == releases && !mistakes);
    return 0;
}
static int interrupted_probe_case(unsigned type, unsigned stage, unsigned cut, unsigned wrap)
{
    ++matrix_cases; matrix_case = 0x40000000u | type | stage << 1 | cut << 4 | wrap << 12;
    reset(true); if (wrap) fake_now = UINT32_MAX - 100;
    announce(1, 0, 9, 1); receiver(3, type != 0, 64); pump(30); CHECK(active_pair(3));
    pressure(stage, 2, 1000); receiver(2, type == 0, 8); pump(cut);
    hold_cancellations = true; unplug(2); receiver(4, type == 0, 8); pump(20);
    CHECK(!early_claims && active_pair(3));
    hold_cancellations = false; pressure(PRESSURE_NONE, 4, 0);
    fake_now += KMS_USB_ALLOC_RETRY_MS; pump(40);
    CHECK(active_pair(4) && active_pair(3) && !owner(2) && !mistakes);
    return clean_all(4, true);
}
static int both_waiting_case(unsigned stage, unsigned first, unsigned wrap)
{
    ++matrix_cases; matrix_case = 0x20000000u | stage | first << 4 | wrap << 5;
    reset(true); if (wrap) fake_now = UINT32_MAX - 100;
    announce(1, 0, 9, 1); pressure(stage, 0, 1000);
    receiver(first ? 3 : 2, first != 0, 8); pump(20);
    receiver(first ? 2 : 3, first == 0, 64); pump(30);
    CHECK(!kms_usb_devices_active() && !mistakes);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        fake_now += KMS_USB_ALLOC_RETRY_MS; pump(30);
        CHECK(!kms_usb_devices_active() && live_parsers() == 0 && !mistakes);
        kms_usb_overdue_t overdue;
        CHECK(!kms_usb_health_overdue(fake_now, KMS_USB_CONTROL_TIMEOUT_MS, &overdue));
    }
    pressure(PRESSURE_NONE, 0, 0); fake_now += KMS_USB_ALLOC_RETRY_MS; pump(40);
    CHECK(active_pair(2) && active_pair(3));
    return clean_all(2, true);
}
static int deferred_channels_case(unsigned extra, unsigned type, unsigned wrap)
{
    ++matrix_cases; matrix_case = 0x10000000u | extra | type << 3 | wrap << 4;
    reset(true); if (wrap) fake_now = UINT32_MAX - 100;
    announce(1, 0, 9, 1); receiver(3, type != 0, 64); pump(30);
    extra_controls = extra; receiver(2, type == 0, 8); pump(30);
    CHECK(active_pair(3) && !active_pair(2) && claim_attempts == 2 && !mistakes);
    int result = sibling_progress(true); if (result) return result;
    extra_controls = 0; pump(30);
    CHECK(active_pair(2) && active_pair(3)); return clean_all(2, true);
}
__declspec(dllexport) unsigned kms_usb_hotplug_case(void) { return matrix_case; }
__declspec(dllexport) unsigned kms_usb_hotplug_cases(void) { return matrix_cases; }
__declspec(dllexport) unsigned kms_usb_hotplug_checks(void) { return checks; }
__declspec(dllexport) int kms_run_usb_hotplug_tests(void)
{
    checks = matrix_cases = 0;
    for (unsigned type = 0; type < 2; ++type)
        for (unsigned other = 0; other < 2; ++other)
            for (unsigned stage = 0; stage < PRESSURE_STAGES; ++stage)
                for (unsigned duration = 0; duration < (stage ? 3u : 1u); ++duration)
                    for (unsigned ordering = 0; ordering < 3; ++ordering)
                        for (unsigned wrap = 0; wrap < 2; ++wrap)
                            for (unsigned mps = 8; mps <= 64; mps += 56) {
                                int result = lifecycle_case(type, other, stage, duration, ordering, wrap, mps);
                                if (result) return result;
                            }
    for (unsigned stage = 1; stage < PRESSURE_STAGES; ++stage)
        for (unsigned action = 0; action < 4; ++action)
            for (unsigned wrap = 0; wrap < 2; ++wrap) {
                int result = intervention_case(stage, action, wrap); if (result) return result;
            }
    static const unsigned cuts[] = {0, 1, 2, 3, 4, 5, 8, 12, 20};
    for (unsigned type = 0; type < 2; ++type)
        for (unsigned stage = 0; stage < PRESSURE_STAGES; ++stage)
            for (unsigned cut = 0; cut < sizeof(cuts) / sizeof(*cuts); ++cut)
                for (unsigned wrap = 0; wrap < 2; ++wrap) {
                    int result = interrupted_probe_case(type, stage, cuts[cut], wrap); if (result) return result;
                }
    for (unsigned stage = 1; stage < PRESSURE_STAGES; ++stage)
        for (unsigned first = 0; first < 2; ++first)
            for (unsigned wrap = 0; wrap < 2; ++wrap) {
                int result = both_waiting_case(stage, first, wrap); if (result) return result;
            }
    for (unsigned extra = 1; extra <= 2; ++extra)
        for (unsigned type = 0; type < 2; ++type)
            for (unsigned wrap = 0; wrap < 2; ++wrap) {
                int result = deferred_channels_case(extra, type, wrap); if (result) return result;
            }
    return 0;
}
