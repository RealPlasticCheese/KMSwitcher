// Compile the production callbacks and polling loop. Only hardware/RTOS APIs
// are replaced; neither command parsing nor USB wire timing is simulated here.
#ifndef KMS_GENERATOR_SOURCE
#define KMS_GENERATOR_SOURCE "../../diagnostics/hid-generator/main/main.c"
#endif
#include KMS_GENERATOR_SOURCE

static bool mounted, suspended, ready, accepts;
static unsigned reports, checks;
static uint32_t reported_sequence;
bool tud_mounted(void) { return mounted; }
bool tud_suspended(void) { return suspended; }
bool tud_hid_n_ready(uint8_t instance) { (void)instance; return ready && mounted && !suspended; }
bool tud_hid_n_report(uint8_t instance, uint8_t id, const void *data, uint16_t n)
{
    (void)instance; (void)id; (void)n;
    const uint8_t *p = data;
    reported_sequence = (uint32_t)p[50] | (uint32_t)p[51] << 8 | (uint32_t)p[52] << 16 | (uint32_t)p[53] << 24;
    if (accepts) ++reports;
    return accepts;
}
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static void reset(void)
{
    mounted = ready = accepts = true; suspended = false;
    run = 1191924403u; source = 2; seed = 1; rate = 30; seconds = 60;
    offered = 1373; submitted = 1325; completed = 1324; missed = 48; digest = 0xca862b12;
    running = false; inflight = true; releasing = release_inflight = detached = false;
    started = next_due = 1000000; last_ping = 1000; reports = 0;
}

__declspec(dllexport) int kms_run_generator_lifecycle_tests(void)
{
    checks = 0;
    // Re-enumeration without a DETACHED callback: the captured failure.
    for (int event_id = TINYUSB_EVENT_ATTACHED; event_id <= TINYUSB_EVENT_DETACHED; ++event_id) {
        for (unsigned active = 0; active < 2; ++active) {
            for (unsigned release = 0; release < 2; ++release) {
                reset(); running = active != 0; release_inflight = release != 0;
                tinyusb_event_t event = {.id = event_id}; usb_event(&event, 0);
                CHECK(!inflight); CHECK(detached);
                CHECK(usb_busy()); // No config/start until neutral recovery begins.
                // A callback arriving before the new neutral submission cannot
                // convert the cancelled report into a successful completion.
                tud_hid_report_complete_cb(0, payload, sizeof(payload));
                CHECK(completed == 1324 && digest == 0xca862b12);
                ready = false; poll(1000000);
                CHECK(!running && releasing && !inflight && !detached);
                CHECK(reports == 0);
                ready = true; accepts = false; poll(1001000);
                CHECK(releasing && !inflight && reports == 0);
                accepts = true; poll(1002000);
                CHECK(!releasing && inflight && release_inflight);
                CHECK(reports == 1 && reported_sequence == 0);
                tud_hid_report_complete_cb(0, payload, sizeof(payload));
                CHECK(!inflight && completed == 1324 && digest == 0xca862b12);
                // Config/start guards are now clear. The missed report remains
                // missed, and the next run may start with fresh counters.
                CHECK(!running && !releasing && !usb_busy());
            }
        }
    }
    // Suspend/resume does not cancel endpoint ownership like reconfiguration.
    for (int event_id = TINYUSB_EVENT_SUSPENDED; event_id <= TINYUSB_EVENT_RESUMED; ++event_id) {
        reset(); tinyusb_event_t event = {.id = event_id}; usb_event(&event, 0);
        CHECK(inflight && !detached);
    }
    // Ordinary data completion still contributes exactly one report and CRC.
    reset(); kms_generator_pattern(payload, source, run, 1325, seed, 1);
    uint32_t expected = kms_crc32_extend(digest, payload, sizeof(payload));
    tud_hid_report_complete_cb(0, payload, sizeof(payload));
    CHECK(completed == 1325 && digest == expected && !inflight);
    tud_hid_report_complete_cb(0, payload, sizeof(payload));
    CHECK(completed == 1325 && digest == expected);
    // Stopping a stalled run cannot falsely clear pending before remount.
    reset(); stop(); poll(1000000);
    CHECK(inflight && releasing && reports == 0 && completed == 1324);
    // Mixed soak uses the ordinary configuration protocol with a new profile.
    reset(); inflight = false;
    CHECK(configure(42, 2, 1, 200, 3605, KMS_GEN_MIXED));
    CHECK(run == 42 && source == 2 && rate == 200 && seconds == 3605 && profile == KMS_GEN_MIXED);
    CHECK(!configure(43, 2, 1, 200, 3605, KMS_GEN_MIXED + 1));
    CHECK(run == 42 && profile == KMS_GEN_MIXED);
    running = true; submitted = 0; poll(1000000);
    CHECK(inflight && reported_sequence == 1 && payload[37] != 0 && payload[1] == 0);
    return 0;
}
__declspec(dllexport) unsigned kms_generator_lifecycle_check_count(void) { return checks; }
