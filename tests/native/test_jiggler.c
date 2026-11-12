#include "kms_jiggler.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, toggles, cycles, insert_reports, j_reports, k_reports, selections, trigger_reports, trigger_usage;
static uint32_t clock;
static bool toggle_ok;
static bool toggle(void *ctx) { (void)ctx; ++toggles; return toggle_ok; }
static bool cycle(void *ctx) { (void)ctx; ++cycles; return toggle_ok; }
static bool select_slot(void *ctx, unsigned slot) { (void)ctx; (void)slot; ++selections; return true; }
static unsigned configured_usage(const kms_router_t *r)
{
    static const unsigned usages[] = {0, 0xe0, 0xe2, 0xe1, KMS_HID_KEY_SCROLL_LOCK,
        KMS_HID_KEY_INSERT, KMS_HID_KEY_PAUSE, KMS_HID_KEY_NUM_LOCK};
    return usages[r->trigger];
}
static void emit(void *ctx, const kms_state_t *s, int32_t x, int32_t y, int32_t w, int32_t p)
{
    (void)ctx; (void)x; (void)y; (void)w; (void)p;
    if (kms_key(s, KMS_HID_KEY_INSERT)) ++insert_reports;
    if (kms_key(s, KMS_HID_KEY_J)) ++j_reports;
    if (kms_key(s, KMS_HID_KEY_K)) ++k_reports;
    if (trigger_usage >= 0xe0 && trigger_usage <= 0xe7) {
        if (s->modifiers & (1u << (trigger_usage - 0xe0))) ++trigger_reports;
    } else if (kms_key(s, trigger_usage)) ++trigger_reports;
}
static void press(kms_state_t *s, unsigned usage)
{
    if (usage >= 0xe0 && usage <= 0xe7) s->modifiers |= (uint8_t)(1u << (usage - 0xe0));
    else if (usage) kms_set_key(s, usage, true);
}
static void key(kms_router_t *r, unsigned input, unsigned first, unsigned second)
{
    kms_sample_t s = {0};
    press(&s.state, first); press(&s.state, second);
    kms_router_input(r, input, &s, clock); clock += 20;
}
static void prefix(kms_router_t *r, unsigned input)
{
    key(r, input, configured_usage(r), 0); key(r, input, 0, 0);
    key(r, input, configured_usage(r), 0); key(r, input, 0, 0);
}
static int32_t motion(const kms_jiggler_t *j, uint32_t now)
{
    kms_jiggler_motion_t movement;
    return kms_jiggler_motion(j, now, &movement) ? movement.x : 0;
}
static void status(kms_packet_t *out, const kms_jiggler_t *j, uint32_t boot)
{
    *out = (kms_packet_t){.type = KMS_STATUS, .session = boot,
        .flags = KMS_FLAG_JIGGLER_SUPPORTED | KMS_FLAG_JIGGLER_DISTANCE | KMS_FLAG_JIGGLER_MODES | (j->mode << KMS_JIGGLER_MODE_SHIFT) | (j->enabled ? KMS_FLAG_JIGGLER_ENABLED : 0),
        .motion = {j->interval_seconds ? j->interval_seconds : 30, j->session, j->sequence, j->distance ? j->distance : 1}};
}
__declspec(dllexport) int kms_run_jiggler_tests(void)
{
    checks = 0;
    for (unsigned color = KMS_COLOR_OFF; color <= KMS_COLOR_WHITE; ++color) {
        unsigned pulse = color ? KMS_COLOR_OFF : KMS_COLOR_BLUE;
        CHECK(kms_jiggler_indicator(0, color) == pulse);
        CHECK(kms_jiggler_indicator(149, color) == pulse);
        CHECK(kms_jiggler_indicator(150, color) == color);
        CHECK(kms_jiggler_indicator(4999, color) == color);
        CHECK(kms_jiggler_indicator(5000, color) == pulse);
        CHECK(kms_jiggler_indicator(5149, color) == pulse);
        CHECK(kms_jiggler_indicator(5150, color) == color);
        CHECK(kms_jiggler_indicator(10000, color) == pulse);
    }
    kms_jiggler_t j = {0};
    kms_packet_t p = {.type = KMS_JIGGLER, .session = 42, .receiver = 7, .slot = 1,
        .sequence = 1, .flags = KMS_FLAG_JIGGLER_ENABLED, .motion = {30}};
    CHECK(!motion(&j, 100000));
    CHECK(kms_jiggler_configure(&j, &p, 100));
    CHECK(!motion(&j, 30099));
    CHECK(motion(&j, 30100) == -1);
    // A rejected/busy USB submission keeps the same direction until accepted.
    CHECK(motion(&j, 30110) == -1);
    kms_jiggler_moved(&j, 30110);
    CHECK(!motion(&j, 60109));
    CHECK(motion(&j, 60110) == 1);
    kms_jiggler_moved(&j, 60110);
    // Duplicate configuration retries never restart a running timer.
    CHECK(kms_jiggler_configure(&j, &p, 90100));
    CHECK(motion(&j, 90110) == -1);
    ++p.sequence; p.motion[0] = 1;
    CHECK(kms_jiggler_configure(&j, &p, 100000));
    CHECK(!motion(&j, 100999) && motion(&j, 101000) == -1);
    kms_jiggler_moved(&j, 101000);
    CHECK(motion(&j, 500000) == 1);
    kms_jiggler_moved(&j, 500000);
    CHECK(!motion(&j, 500001)); // No catch-up burst after a suspended PC.
    kms_jiggler_activity(&j, 500900);
    CHECK(j.enabled && !j.right && !motion(&j, 501899));
    CHECK(motion(&j, 501900) == -1);
    kms_jiggler_activity(&j, 501900);
    CHECK(!motion(&j, 502899) && motion(&j, 502900) == -1);
    p.sequence = 1; p.flags = 0;
    CHECK(!kms_jiggler_configure(&j, &p, 500010) && j.enabled);
    p.sequence = 2;
    CHECK(!kms_jiggler_configure(&j, &p, 500020) && j.enabled); // Same ID, different payload.
    p.sequence = 3;
    CHECK(kms_jiggler_configure(&j, &p, 500030) && !motion(&j, 900000));
    ++p.sequence; p.flags = KMS_FLAG_JIGGLER_ENABLED;
    CHECK(kms_jiggler_configure(&j, &p, 0xffffff00u));
    CHECK(!motion(&j, 743) && motion(&j, 744) == -1);
    p.motion[0] = 0; CHECK(!kms_jiggler_configure(&j, &p, 900));
    p.motion[0] = 65536; CHECK(!kms_jiggler_configure(&j, &p, 900));
    p.motion[0] = 65535; ++p.sequence;
    CHECK(kms_jiggler_configure(&j, &p, 1000));
    CHECK(!motion(&j, 65535999) && motion(&j, 65536000) == -1);
    p.type = KMS_RELEASE; CHECK(!kms_jiggler_configure(&j, &p, 2000) && j.enabled);
    kms_jiggler_t scaled = {0};
    kms_packet_t config = {.type = KMS_JIGGLER, .session = 42, .sequence = 1,
        .flags = KMS_FLAG_JIGGLER_ENABLED, .motion = {1, 0, 0, 127}};
    CHECK(kms_jiggler_configure(&scaled, &config, 100));
    CHECK(motion(&scaled, 1100) == -127);
    kms_jiggler_moved(&scaled, 1100); CHECK(motion(&scaled, 2100) == 127);
    ++config.sequence; config.motion[3] = 128; CHECK(!kms_jiggler_configure(&scaled, &config, 2100));
    config.motion[3] = 0; CHECK(kms_jiggler_configure(&scaled, &config, 2200) && scaled.distance == 1);
    CHECK(motion(&scaled, 3200) == 1); // Old S3 default, direction retained.
    // Controller retries are absolute settings, with an independent state per dongle.
    kms_jiggler_sync_t sync[2] = {0}; kms_jiggler_t receivers[2] = {0};
    kms_packet_t report, command, decoded; status(&report, &receivers[0], 7);
    kms_jiggler_sync_status(&sync[0], &report, 42, 30, 1);
    status(&report, &receivers[1], 8); kms_jiggler_sync_status(&sync[1], &report, 42, 30, 1);
    CHECK(sync[0].known && sync[1].known && !sync[0].pending && !sync[1].enabled);
    kms_jiggler_sync_request(&sync[0], true, 30, 1, KMS_JIGGLER_LEFT_RIGHT);
    CHECK(kms_jiggler_sync_packet(&sync[0], 42, 1, 10, &command));
    uint8_t wire[KMS_WIRE_SIZE];
    CHECK(kms_wire_encode(&command, wire, sizeof(wire)) == sizeof(wire));
    CHECK(kms_wire_decode(&decoded, wire, sizeof(wire)) && decoded.type == KMS_JIGGLER && decoded.motion[0] == 30);
    sync[0].sent = true; sync[0].sent_at = 10;
    CHECK(!kms_jiggler_sync_packet(&sync[0], 42, 1, 10 + KMS_STATUS_MS - 1, &command));
    CHECK(kms_jiggler_sync_packet(&sync[0], 42, 1, 10 + KMS_STATUS_MS, &command));
    CHECK(kms_jiggler_configure(&receivers[0], &command, 1000));
    // Lost status confirmation causes a duplicate retry, without toggling twice.
    CHECK(kms_jiggler_configure(&receivers[0], &command, 2000) && receivers[0].changed == 1000);
    status(&report, &receivers[0], 7);
    kms_jiggler_sync_status(&sync[0], &report, 42, 30, 1);
    CHECK(!sync[0].pending && sync[0].enabled && !sync[1].enabled);
    kms_packet_t stale = report; stale.motion[2] = 0; stale.flags &= (uint8_t)~KMS_FLAG_JIGGLER_ENABLED;
    kms_jiggler_sync_status(&sync[0], &stale, 42, 30, 1);
    CHECK(sync[0].enabled && !sync[0].pending);
    CHECK(!kms_jiggler_sync_packet(&sync[0], 42, 1, 3000, &command));
    // Releasing the ordinary route and letting its lease expire does not stop the jiggler.
    kms_receiver_guard_t guard = {0}; kms_sample_t sample; bool apply;
    kms_packet_t route = {.type = KMS_ACTIVATE, .session = 42, .generation = 1, .sequence = 1};
    CHECK(kms_receiver_accept(&guard, &route, 2000, &sample, &apply) && apply);
    route.type = KMS_RELEASE; ++route.sequence;
    CHECK(kms_receiver_accept(&guard, &route, 2100, &sample, &apply) && !guard.active);
    CHECK(motion(&receivers[0], 31000) == -1 && !motion(&receivers[1], 31000));
    kms_jiggler_moved(&receivers[0], 31000);
    CHECK(motion(&receivers[0], 61000) == 1);
    // Updating an inactive dongle's interval preserves its enabled state.
    kms_jiggler_sync_request(&sync[0], sync[0].enabled, 1, 1, KMS_JIGGLER_LEFT_RIGHT);
    CHECK(kms_jiggler_sync_packet(&sync[0], 42, 1, 62000, &command));
    CHECK(kms_jiggler_configure(&receivers[0], &command, 62000));
    status(&report, &receivers[0], 7); kms_jiggler_sync_status(&sync[0], &report, 42, 1, 1);
    CHECK(!sync[0].pending && receivers[0].enabled && motion(&receivers[0], 63000) == 1);
    // New receiver boots start disabled. Old firmware never receives the new command.
    receivers[0] = (kms_jiggler_t){0}; status(&report, &receivers[0], 9);
    kms_jiggler_sync_status(&sync[0], &report, 42, 30, 1);
    CHECK(sync[0].known && !sync[0].enabled && sync[0].boot == 9);
    kms_jiggler_sync_request(&sync[0], true, 1, 1, KMS_JIGGLER_LEFT_RIGHT);
    report.session = 10; kms_jiggler_sync_status(&sync[0], &report, 42, 1, 1);
    CHECK(!sync[0].enabled && sync[0].pending); // A pending enable cannot cross an S2 restart.
    CHECK(kms_jiggler_sync_packet(&sync[0], 42, 1, 64000, &command));
    CHECK(command.receiver == 10 && !command.flags && command.motion[0] == 1);
    CHECK(kms_jiggler_configure(&receivers[0], &command, 64000) && !receivers[0].enabled);
    report.flags = 0; kms_jiggler_sync_status(&sync[0], &report, 42, 30, 1);
    CHECK(!sync[0].known && !kms_jiggler_sync_packet(&sync[0], 42, 1, 64000, &command));
    // Distance changes need exact S2 confirmation, and legacy S2s stay at one count.
    kms_jiggler_sync_t distance_sync = {0}; status(&report, &scaled, 11);
    kms_jiggler_sync_status(&distance_sync, &report, 42, 1, 4);
    CHECK(distance_sync.pending && distance_sync.distance == 4 && distance_sync.distance_supported);
    CHECK(kms_jiggler_sync_packet(&distance_sync, 42, 1, 0, &command) && command.motion[3] == 4);
    CHECK(kms_jiggler_configure(&scaled, &command, 3300));
    status(&report, &scaled, 11); report.motion[3] = 1;
    kms_jiggler_sync_status(&distance_sync, &report, 42, 1, 4); CHECK(distance_sync.pending);
    report.motion[3] = 4; kms_jiggler_sync_status(&distance_sync, &report, 42, 1, 4); CHECK(!distance_sync.pending);
    distance_sync = (kms_jiggler_sync_t){0}; report.flags &= (uint8_t)~KMS_FLAG_JIGGLER_DISTANCE; report.motion[3] = 0;
    kms_jiggler_sync_status(&distance_sync, &report, 42, 1, 4);
    CHECK(distance_sync.known && !distance_sync.pending && distance_sync.distance == 1 && !distance_sync.distance_supported);
    // The jiggler follows every configured trigger, on either keyboard.
    for (unsigned trigger = KMS_TRIGGER_CTRL; trigger <= KMS_TRIGGER_NUM_LOCK; ++trigger)
        for (unsigned input = 0; input < 2; ++input)
            for (unsigned overlap = 0; overlap < 3; ++overlap) {
                static kms_router_t r;
                kms_router_init(&r, emit, select_slot, NULL, NULL); r.toggle_jiggler = toggle;
                r.cycle_jiggler_mode = cycle;
                CHECK(kms_router_set_trigger(&r, (kms_trigger_t)trigger));
                toggles = cycles = insert_reports = j_reports = k_reports = selections = 0; toggle_ok = true; clock = 100;
                trigger_usage = configured_usage(&r); trigger_reports = 0;
                key(&r, input, trigger_usage, 0); key(&r, input, 0, 0);
                key(&r, input, trigger_usage, 0);
                if (overlap == 0) key(&r, input, 0, 0);
                key(&r, input, KMS_HID_KEY_J, overlap == 1 ? trigger_usage : 0);
                key(&r, input, overlap == 1 ? trigger_usage : 0, 0); key(&r, input, 0, 0);
                CHECK(toggles == 1 && !insert_reports && !j_reports && !selections && !trigger_reports);
                CHECK(kms_router_take_notice(&r) == KMS_NOTICE_SAVED);
                prefix(&r, input); key(&r, input, KMS_HID_KEY_J, 0); key(&r, input, 0, 0);
                CHECK(toggles == 2 && !insert_reports && !j_reports && !trigger_reports);
                prefix(&r, input); key(&r, input, KMS_HID_KEY_K, 0);
                key(&r, input, KMS_HID_KEY_K, 0); key(&r, input, 0, 0); // Held/repeated K cycles only once.
                CHECK(cycles == 1 && toggles == 2 && !k_reports && !trigger_reports);
                CHECK(kms_router_take_notice(&r) == KMS_NOTICE_SAVED);
                key(&r, input, trigger_usage, 0); key(&r, input, 0, 0);
                key(&r, input, trigger_usage, 0);
                if (overlap == 0) key(&r, input, 0, 0);
                key(&r, input, KMS_HID_KEY_K, overlap == 1 ? trigger_usage : 0);
                key(&r, input, overlap == 1 ? trigger_usage : 0, 0); key(&r, input, 0, 0);
                CHECK(cycles == 2 && !k_reports && !trigger_reports && !insert_reports);
            }
    static kms_router_t r;
    kms_router_init(&r, emit, select_slot, NULL, NULL); r.toggle_jiggler = toggle;
    CHECK(kms_router_set_trigger(&r, KMS_TRIGGER_CTRL));
    toggles = insert_reports = j_reports = selections = 0; clock = 100;
    key(&r, 0, KMS_HID_KEY_INSERT, 0); key(&r, 0, 0, 0);
    key(&r, 0, KMS_HID_KEY_INSERT, 0); key(&r, 0, 0, 0);
    key(&r, 0, KMS_HID_KEY_J, 0); key(&r, 0, 0, 0);
    CHECK(!toggles && insert_reports == 2 && j_reports == 1 && r.mode == KMS_MODE_NORMAL);
    CHECK(kms_router_set_trigger(&r, KMS_TRIGGER_INSERT));
    toggles = insert_reports = j_reports = selections = 0;
    prefix(&r, 0); kms_router_tick(&r, clock + r.select_ms);
    CHECK(insert_reports == 2 && !toggles); // Ordinary Insert is replayed on timeout.
    insert_reports = 0; clock += r.select_ms + 100;
    prefix(&r, 0); key(&r, 0, KMS_HID_KEY_ESCAPE, 0); key(&r, 0, 0, 0);
    CHECK(!insert_reports && !toggles);
    prefix(&r, 0); toggle_ok = false; key(&r, 0, KMS_HID_KEY_J, 0); key(&r, 0, 0, 0);
    CHECK(kms_router_take_notice(&r) == KMS_NOTICE_ERROR && !j_reports);
    r.cycle_jiggler_mode = cycle; k_reports = cycles = 0;
    prefix(&r, 0); key(&r, 0, KMS_HID_KEY_K, 0); key(&r, 0, 0, 0);
    CHECK(kms_router_take_notice(&r) == KMS_NOTICE_ERROR && cycles == 1 && !k_reports);
    key(&r, 0, KMS_HID_KEY_K, 0); key(&r, 0, 0, 0);
    CHECK(k_reports == 1 && cycles == 1); // Ordinary K remains ordinary input.
    return 0;
}
__declspec(dllexport) int kms_jiggler_test_count(void) { return checks; }
