#include "kms_desktop.h"
#include "kms_desktop_routes.h"
#include <string.h>

static kms_desktop_master_t model;
static kms_desktop_routes_t routing;
static kms_desktop_message_t last, event;
static unsigned checks, selected, saves, screen_saves;
static uint8_t saved_screens[KMS_D_SCREENS_BYTES];
static uint32_t request_time;
static bool storage_ok, selection_ok;
static uint8_t unavailable_slots;
#define CHECK(x) do { ++checks; if (!(x)) return __LINE__; } while (0)
static bool send_message(void *ctx, unsigned slot, const kms_desktop_message_t *m)
{ (void)ctx; (void)slot; if (m->op == KMS_D_PREPARE) event = *m; else last = *m; return true; }
static bool save(void *ctx, const kms_desktop_config_t *data, const kms_desktop_master_t *m, uint8_t mask)
{ (void)ctx; (void)data; (void)m; (void)mask; ++saves; return storage_ok; }
static bool save_screens(void *ctx, unsigned slot, const uint8_t *data)
{ (void)ctx; (void)slot; ++screen_saves; if (!storage_ok) return false; memcpy(saved_screens, data, sizeof(saved_screens)); return true; }
static bool select_station(void *ctx, unsigned slot)
{ (void)ctx; if (!selection_ok) return false; selected = slot; return true; }
static bool station_available(void *ctx, unsigned slot)
{ (void)ctx; return !(unavailable_slots & (1u << (slot - 1))); }
static void fixture(void)
{
    const kms_desktop_io_t io = {.send = send_message, .save = save, .save_screens = save_screens,
        .select = select_station, .available = station_available};
    kms_desktop_io_t configured = io; configured.routes = &routing;
    kms_desktop_master_init(&model, &configured, 101); selected = saves = 0; storage_ok = true;
    selection_ok = true; unavailable_slots = 0; screen_saves = 0; request_time = 1000; memset(&event, 0, sizeof(event));
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        kms_desktop_peer_t *p = &model.peers[i];
        p->boot = 10; p->session = i + 10; p->seen = 1000; p->ready = p->enabled = true; p->count = 1; p->geometry = 1;
        p->screens[0] = (kms_desktop_rect_t){0, 0, 100, 100}; model.config.stations[i].configured = true;
    }
    model.config.stations[1].x = 100; model.config.stations[2].x = 200;
    model.config.stations[3].y = 100; model.config.stations[4].y = 200;
    model.config.stations[5].y = -100; model.config.stations[6].x = -100;
    kms_desktop_master_tick(&model, 1, 77, true, 1000);
}
static void request(unsigned slot, unsigned op, unsigned id, unsigned a, unsigned b, int x, int y)
{
    kms_desktop_message_t q = {.op = (uint8_t)op, .id = id, .session = model.peers[slot - 1].session};
    kms_put_u32(q.data, a); kms_put_u32(q.data + 4, b);
    kms_put_u32(q.data + 8, (uint32_t)x); kms_put_u32(q.data + 12, (uint32_t)y); kms_put_u32(q.data + 16, 1);
    kms_desktop_master_request(&model, slot, 10, &q, request_time);
}
static void sleeping_neighbor(void)
{
    fixture(); request_time = 4000; model.peers[0].seen = request_time;
    for (unsigned i = 2; i < KMS_SLOTS; ++i) model.config.stations[i].configured = false;
    kms_desktop_master_tick(&model, 1, 77, true, request_time);
    request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
}
static void inside_corner(void)
{
    fixture();
    for (unsigned i = 2; i < KMS_SLOTS; ++i) model.config.stations[i].configured = false;
    model.peers[0].screens[0] = (kms_desktop_rect_t){0, 0, 1366, 768};
    model.config.stations[0].x = 554; model.config.stations[0].y = 192;
    model.config.stations[1].x = model.config.stations[1].y = 0;
    model.peers[1].screens[0] = (kms_desktop_rect_t){0, 960, 1920, 1200};
    model.peers[1].screens[1] = (kms_desktop_rect_t){1920, 0, 3840, 2160}; model.peers[1].count = 2;
}
static void report_screens(unsigned slot, unsigned version, const kms_desktop_rect_t *screens, unsigned count)
{
    request(slot, KMS_D_SCREENS_BEGIN, 300, version, count, 0, 0);
    for (unsigned i = 0; i < count; ++i) {
        kms_desktop_message_t q = {.op = KMS_D_SCREEN, .id = 301 + i, .session = model.peers[slot - 1].session};
        kms_put_u32(q.data, version); kms_put_u32(q.data + 4, i);
        kms_put_u32(q.data + 8, (uint32_t)screens[i].x); kms_put_u32(q.data + 12, (uint32_t)screens[i].y);
        kms_put_u32(q.data + 16, (uint32_t)screens[i].width); kms_put_u32(q.data + 20, (uint32_t)screens[i].height);
        kms_desktop_master_request(&model, slot, 10, &q, request_time);
    }
    request(slot, KMS_D_SCREENS_COMMIT, 320, version, 0, 0, 0);
}
static int screen_persistence_tests(void)
{
    fixture();
    kms_desktop_rect_t screens[] = {{-1920, 960, 1920, 1200}, {0, 0, 3840, 2160}};
    report_screens(2, 7, screens, 2);
    CHECK(last.result == KMS_D_OK && screen_saves == 1 && model.peers[1].count == 2);
    CHECK(model.revision == 101); // Screen reports must not invalidate placement edits.
    kms_desktop_peer_t restored = {0};
    CHECK(kms_desktop_screens_decode(&restored, saved_screens, sizeof(saved_screens)));
    CHECK(restored.count == 2 && !restored.geometry && !restored.session && !restored.ready);
    CHECK(!memcmp(restored.screens, screens, sizeof(screens)));
    for (unsigned i = 0; i < sizeof(saved_screens); ++i) {
        saved_screens[i] ^= 1;
        CHECK(!kms_desktop_screens_decode(&restored, saved_screens, sizeof(saved_screens)));
        saved_screens[i] ^= 1;
    }
    CHECK(!kms_desktop_screens_decode(&restored, saved_screens, sizeof(saved_screens) - 1));
    CHECK(restored.count == 2 && !memcmp(restored.screens, screens, sizeof(screens)));
    report_screens(2, 8, screens, 2); CHECK(screen_saves == 1 && model.peers[1].geometry == 8);
    kms_desktop_message_t hello = {.op = KMS_D_HELLO, .id = 400, .session = 999};
    kms_desktop_master_request(&model, 2, 10, &hello, request_time);
    CHECK(model.peers[1].count == 2 && !model.peers[1].geometry && !model.peers[1].enabled && !model.peers[1].ready);
    request(1, KMS_D_PEER, 401, 2, 0, 0, 0);
    CHECK(!last.data[4] && last.data[5] == 2 && (int32_t)kms_get_u32(last.data + 16) == -1920);
    request(2, KMS_D_SCREENS_BEGIN, 402, 1, 2, 0, 0);
    request(2, KMS_D_SCREENS_COMMIT, 403, 1, 0, 0, 0);
    CHECK(last.result == KMS_D_INVALID && model.peers[1].count == 2 && !model.peers[1].geometry);
    report_screens(2, 2, screens, 2); CHECK(screen_saves == 1 && model.peers[1].geometry == 2);
    screens[0].height = 1080; storage_ok = false;
    report_screens(2, 3, screens, 2);
    CHECK(last.result == KMS_D_STORAGE && model.peers[1].screens[0].height == 1200 && model.peers[1].geometry == 2);
    storage_ok = true; request(2, KMS_D_SCREENS_COMMIT, 404, 3, 0, 0, 0);
    CHECK(last.result == KMS_D_OK && model.peers[1].screens[0].height == 1080 && model.peers[1].geometry == 3);
    // Restored geometry remains visible after reboot but cannot initiate a handoff.
    fixture(); model.peers[1] = restored; model.config.stations[2].configured = false;
    request(1, KMS_D_CROSS, 405, 77, KMS_D_RIGHT, 99, 50);
    CHECK(last.result == KMS_D_OFFLINE && !model.handoff.active);
    request(1, KMS_D_PEER, 406, 2, 1, 0, 0);
    CHECK(!last.data[4] && last.data[5] == 2 && kms_get_u32(last.data + 24) == 3840);
    return 0;
}
static void offline_line(unsigned direction, unsigned condition)
{
    fixture(); request_time = 4000;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        model.config.stations[i].configured = i < 3;
        model.config.stations[i].x = direction == KMS_D_LEFT ? -(int32_t)i * 100 : direction == KMS_D_RIGHT ? (int32_t)i * 100 : 0;
        model.config.stations[i].y = direction == KMS_D_UP ? -(int32_t)i * 100 : direction == KMS_D_DOWN ? (int32_t)i * 100 : 0;
        model.peers[i].seen = request_time;
    }
    if (condition == 0) { // Persisted screens with no helper since master reboot.
        model.peers[1].session = model.peers[1].geometry = 0;
        model.peers[1].enabled = model.peers[1].ready = false;
    }
    if (condition == 1) unavailable_slots = 1u << 1; // Powered dongle, unmounted USB host or no radio.
    if (condition == 2) model.peers[1].enabled = false;
    if (condition == 3) model.peers[1].ready = false;
    if (condition >= 4) model.peers[1].seen = 1000;
    if (condition == 5) model.peers[1].ready = false;
    kms_desktop_master_tick(&model, 1, 77, true, request_time);
}
static void line_cross(unsigned direction)
{
    request(1, KMS_D_CROSS, 500, 77, direction,
        direction == KMS_D_LEFT ? 0 : direction == KMS_D_RIGHT ? 99 : 50,
        direction == KMS_D_UP ? 0 : direction == KMS_D_DOWN ? 99 : 50);
}
static int offline_skipping_tests(void)
{
    for (unsigned direction = KMS_D_LEFT; direction <= KMS_D_DOWN; ++direction) {
        for (unsigned condition = 0; condition < 6; ++condition) {
            offline_line(direction, condition); line_cross(direction);
            CHECK(last.result == KMS_D_OK && model.handoff.active && model.handoff.target == 3 && !selected);
            CHECK(model.handoff.landing_x == (direction == KMS_D_LEFT ? 97 : direction == KMS_D_RIGHT ? 2 : 50));
            CHECK(model.handoff.landing_y == (direction == KMS_D_UP ? 97 : direction == KMS_D_DOWN ? 2 : 50));
            kms_desktop_master_tick(&model, 1, 77, true, request_time + 1);
            CHECK(event.op == KMS_D_PREPARE && event.session == model.peers[2].session);
            request(3, KMS_D_READY, 501, model.handoff.id, 0, 0, 0);
            CHECK(selected == 3 && !model.handoff.active);
        }
        // A remaining gap is repaired with a reciprocal virtual connection.
        for (unsigned gap = 0; gap < 2; ++gap) {
            offline_line(direction, 0);
            unsigned i = gap ? 2 : 1;
            model.config.stations[i].x += direction == KMS_D_LEFT ? -1 : direction == KMS_D_RIGHT ? 1 : 0;
            model.config.stations[i].y += direction == KMS_D_UP ? -1 : direction == KMS_D_DOWN ? 1 : 0;
            line_cross(direction); CHECK(last.result == KMS_D_OK && model.handoff.target == 3 && !selected);
        }
    }
    // An unavailable station's whole area is bypassed, including an internal hole.
    offline_line(KMS_D_RIGHT, 0); model.peers[1].count = 2;
    model.peers[1].screens[0] = (kms_desktop_rect_t){0, 0, 40, 100};
    model.peers[1].screens[1] = (kms_desktop_rect_t){60, 0, 40, 100};
    line_cross(KMS_D_RIGHT); CHECK(last.result == KMS_D_OK && model.handoff.target == 3);
    // A disconnected source monitor must also have a route to another PC.
    offline_line(KMS_D_RIGHT, 0); model.peers[0].count = 2;
    model.peers[0].screens[1] = (kms_desktop_rect_t){200, 0, 100, 100};
    model.peers[1].screens[0].width = 200; model.config.stations[2].x = 300;
    line_cross(KMS_D_RIGHT); CHECK(last.result == KMS_D_OFFLINE && !model.handoff.active);
    line_cross(KMS_D_DOWN); CHECK(last.result == KMS_D_OK && model.handoff.target == 3);
    // A live adjacent desktop still gets priority over the more distant one.
    offline_line(KMS_D_RIGHT, 6); model.peers[1].seen = request_time;
    line_cross(KMS_D_RIGHT); CHECK(model.handoff.target == 2);
    uint32_t late = model.handoff.id;
    kms_desktop_master_tick(&model, 1, 77, true, request_time + KMS_D_HANDOFF_MS);
    CHECK(!model.handoff.active && !selected); // An awake PC is still a barrier after an ACK timeout.
    request(2, KMS_D_READY, 502, late, 0, 0, 0); CHECK(last.result == KMS_D_STALE && !selected);
    request(3, KMS_D_READY, 503, late, 0, 0, 0); CHECK(last.result == KMS_D_STALE && !selected);
    offline_line(KMS_D_RIGHT, 6); model.peers[1].seen = request_time;
    line_cross(KMS_D_RIGHT); CHECK(model.handoff.target == 2);
    request(2, KMS_D_READY, 508, model.handoff.id, 1, 0, 0);
    CHECK(!model.handoff.active && !selected); // Declining arrival also cannot turn an awake PC into a shortcut.
    // With no running destination beyond, an automatic crossing stops.
    offline_line(KMS_D_RIGHT, 4); model.config.stations[2].configured = false;
    line_cross(KMS_D_RIGHT); CHECK(last.result == KMS_D_OFFLINE && !model.handoff.active);
    kms_desktop_master_tick(&model, 1, 77, true, request_time + 1); CHECK(!selected);
    offline_line(KMS_D_RIGHT, 1); model.config.stations[2].configured = false;
    line_cross(KMS_D_RIGHT); CHECK(last.result == KMS_D_OFFLINE && !selected);
    // Traverse all five intermediate stations, including reverse slot order.
    fixture();
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        model.config.stations[i].x = (int32_t)i * 100; model.config.stations[i].y = 0;
        if (i && i < KMS_SLOTS - 1) model.peers[i].session = model.peers[i].geometry = 0;
    }
    kms_desktop_master_tick(&model, 7, 77, true, request_time);
    request(7, KMS_D_CROSS, 504, 77, KMS_D_LEFT, 0, 50);
    CHECK(model.handoff.active && model.handoff.target == 1 && model.handoff.landing_x == 97);
    // Reported three-dongle arrangement: 1 sits in 2's empty corner, with a
    // 277-unit gap to 2's external monitor. Bypass works both ways while 2 is off.
    inside_corner();
    model.config.stations[0].x = 277;
    model.peers[1].session = model.peers[1].geometry = 0;
    model.peers[1].enabled = model.peers[1].ready = false;
    model.peers[2].screens[0] = (kms_desktop_rect_t){0, 0, 3840, 2160};
    model.config.stations[2] = (kms_desktop_station_t){.x = 5760, .configured = true};
    kms_desktop_master_tick(&model, 3, 77, true, request_time);
    request(3, KMS_D_CROSS, 505, 77, KMS_D_LEFT, 0, 500);
    CHECK(model.handoff.active && model.handoff.target == 1);
    CHECK(model.handoff.landing_x == 1363 && model.handoff.landing_y >= 2 && model.handoff.landing_y < 766);
    model.handoff.active = false;
    request(3, KMS_D_CROSS, 506, 77, KMS_D_LEFT, 0, 1000);
    CHECK(last.result == KMS_D_OK && model.handoff.target == 1); // Map below the small screen too.
    kms_desktop_master_tick(&model, 1, 77, true, request_time);
    request(1, KMS_D_CROSS, 507, 77, KMS_D_RIGHT, 1365, 300);
    CHECK(model.handoff.active && model.handoff.target == 3 && model.handoff.landing_y >= 2 && model.handoff.landing_y < 2158);
    return 0;
}
static int edge_departure_tests(void)
{
    fixture(); request(1, KMS_D_CROSS, 91, 77, KMS_D_RIGHT, 99, 50);
    request(1, KMS_D_HEARTBEAT, 92, 3, 0, 0, 0);
    CHECK(kms_get_u32(last.data + 16) == 0); // Accepted is not completed.
    request(2, KMS_D_READY, 93, model.handoff.id, 0, 0, 0);
    CHECK(selected == 2);
    kms_desktop_master_tick(&model, 2, 78, true, 1001);
    request(1, KMS_D_HEARTBEAT, 94, 3, 0, 0, 0);
    CHECK(kms_get_u32(last.data + 16) == 91);
    request(2, KMS_D_HEARTBEAT, 95, 3, 0, 0, 0);
    CHECK(kms_get_u32(last.data + 16) == 0); // Receipt belongs to the source session.
    ++model.peers[0].session;
    request(1, KMS_D_HEARTBEAT, 96, 3, 0, 0, 0);
    CHECK(kms_get_u32(last.data + 16) == 0);

    // Keyboard and CLI overrides must not inherit a pending or completed CROSS.
    for (unsigned completed = 0; completed < 2; ++completed) {
        for (unsigned manual = 0; manual < 2; ++manual) {
            fixture(); request(1, KMS_D_CROSS, 91, 77, KMS_D_RIGHT, 99, 50);
            if (completed) {
                request(2, KMS_D_READY, 92, model.handoff.id, 0, 0, 0);
                kms_desktop_master_tick(&model, 2, 78, true, 1001);
            }
            if (manual) request(1, KMS_D_SELECT, 93, 2, 0, 0, 0);
            // A keyboard route change arrives through the regular link tick;
            // even selecting the same destination changes the input generation.
            kms_desktop_master_tick(&model, 2, completed ? 79 : 78, true, 1002);
            request(1, KMS_D_HEARTBEAT, 94, 3, 0, 0, 0);
            CHECK(kms_get_u32(last.data + 16) == 0);
        }
    }
    fixture(); request(1, KMS_D_CROSS, 91, 77, KMS_D_RIGHT, 99, 50);
    selection_ok = false;
    request(2, KMS_D_READY, 92, model.handoff.id, 0, 0, 0);
    CHECK(last.result == KMS_D_BUSY);
    kms_desktop_master_tick(&model, 2, 78, true, 1001);
    request(1, KMS_D_HEARTBEAT, 93, 3, 0, 0, 0);
    CHECK(kms_get_u32(last.data + 16) == 0);

    // No automatic wake fallback and therefore no edge-departure receipt.
    sleeping_neighbor(); kms_desktop_master_tick(&model, 1, 77, true, 4001);
    CHECK(!selected && !model.handoff.active);
    request(1, KMS_D_HEARTBEAT, 93, 3, 0, 0, 0);
    CHECK(kms_get_u32(last.data + 16) == 0);
    return 0;
}

static int scaled_monitor_tests(void)
{
    // Independent saved desk sizes, negative local origins and two monitors per PC.
    for (unsigned d = KMS_D_LEFT; d <= KMS_D_DOWN; ++d) {
        fixture();
        for (unsigned i = 2; i < KMS_SLOTS; ++i) model.config.stations[i].configured = false;
        model.peers[0].count = model.peers[1].count = 2;
        model.peers[0].screens[0] = (kms_desktop_rect_t){-300, -150, 100, 100};
        model.peers[0].screens[1] = (kms_desktop_rect_t){-200, -150, 100, 100};
        model.peers[1].screens[0] = (kms_desktop_rect_t){-900, -800, 400, 200};
        model.peers[1].screens[1] = (kms_desktop_rect_t){-500, -800, 400, 200};
        model.config.stations[0] = (kms_desktop_station_t){.configured = true, .x = -777, .y = -299, .width = 333, .height = 151};
        model.config.stations[1] = (kms_desktop_station_t){.configured = true,
            .x = -777 + (d == KMS_D_RIGHT ? 333 : d == KMS_D_LEFT ? -277 : 0),
            .y = -299 + (d == KMS_D_DOWN ? 151 : d == KMS_D_UP ? -123 : 0), .width = 277, .height = 123};
        request(1, KMS_D_CROSS, 600, 77, d,
            d == KMS_D_LEFT ? -300 : d == KMS_D_RIGHT ? -101 : -250,
            d == KMS_D_UP ? -150 : d == KMS_D_DOWN ? -51 : -100);
        CHECK(model.handoff.active && model.handoff.target == 2);
        CHECK(model.handoff.landing_x >= -898 && model.handoff.landing_x <= -103);
        CHECK(model.handoff.landing_y >= -798 && model.handoff.landing_y <= -603);
        if (d == KMS_D_RIGHT) CHECK(model.handoff.landing_x == -898 && model.handoff.landing_y == -679);
        if (d == KMS_D_LEFT) CHECK(model.handoff.landing_x == -103);
        if (d == KMS_D_UP) CHECK(model.handoff.landing_y == -603);
        if (d == KMS_D_DOWN) CHECK(model.handoff.landing_y == -798);
        request(2, KMS_D_READY, 601, model.handoff.id, 0, 0, 0); CHECK(selected == 2);
    }
    return 0;
}
static unsigned preview_kinds(unsigned slot, unsigned destination)
{
    unsigned kinds = 0;
    uint32_t cursor = 0;
    do {
        request(2, KMS_D_PEER, 550 + cursor, slot, KMS_D_PEER_JOIN_PREVIEW | KMS_D_PEER_ALL_ROUTES | cursor, 0, 0);
        if (last.result != KMS_D_OK || !last.data[4]) break;
        if (last.data[6] == destination) kinds |= 1u << last.data[5];
        uint32_t next = kms_get_u32(last.data + 32);
        if (next <= cursor) break;
        cursor = next;
    } while (true);
    return kinds;
}
static void tolerance_desk(int gap)
{
    inside_corner();
    model.config.stations[0].x = 277;
    model.config.stations[0].y = 192 - gap;
    model.config.stations[2] = (kms_desktop_station_t){.x = 5760, .configured = true};
    model.peers[2].screens[0] = (kms_desktop_rect_t){0, 0, 3840, 2160};
    kms_desktop_master_tick(&model, 2, 77, true, request_time);
}
static int tolerance_tests(void)
{
    const struct { int gap; uint32_t gaps, overlaps; bool bridged; } cases[] = {
        {3, 8, 8, true}, {8, 8, 8, true}, {9, 8, 8, false},
        {-8, 8, 8, true}, {-9, 8, 8, false}, {3, 0, 50, false}, {-3, 50, 0, false},
        {9, 9, 0, true}, {-9, 0, 9, true}, {100, UINT32_MAX, 0, true},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        tolerance_desk(cases[i].gap);
        model.config.gap_tolerance = cases[i].gaps;
        model.config.overlap_tolerance = cases[i].overlaps;
        unsigned target; int32_t x, y;
        CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
        CHECK(kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 900, 960, &target, &x, &y) == cases[i].bridged);
        if (cases[i].bridged) CHECK(target == 1 && x == 623 && y == 765);
        CHECK(kms_desktop_routes_cross(&routing, 2, KMS_D_RIGHT, 5759, 1000, &target, &x, &y) && target == 3);
        unsigned kinds = preview_kinds(2, 1);
        CHECK(kinds & (1u << (cases[i].bridged ? KMS_D_ROUTE_TOLERANCE : KMS_D_ROUTE_FALLBACK)));
        CHECK(preview_kinds(2, 3) == (1u << KMS_D_ROUTE_EXACT));
    }
    tolerance_desk(3);
    unsigned target; int32_t x, y;
    CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
    // The NUC's right edge has a 277-unit gap to the external display.
    CHECK(!kms_desktop_routes_cross(&routing, 1, KMS_D_RIGHT, 1365, 300, &target, &x, &y));
    model.config.gap_tolerance = 277;
    CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
    CHECK(kms_desktop_routes_cross(&routing, 1, KMS_D_RIGHT, 1365, 300, &target, &x, &y) && target == 2);

    tolerance_desk(3);
    model.config.stations[3] = model.config.stations[0];
    model.peers[3].screens[0] = model.peers[0].screens[0];
    kms_desktop_master_tick(&model, 2, 77, true, request_time);
    CHECK(!(preview_kinds(2, 1) & (1u << KMS_D_ROUTE_TOLERANCE)));
    CHECK(!(preview_kinds(2, 4) & (1u << KMS_D_ROUTE_TOLERANCE)));

    // A screen blocking the join in the saved geometry must not create a
    // remembered crossing later just because that screen went offline.
    tolerance_desk(20);
    model.config.stations[3] = (kms_desktop_station_t){.x = 277, .y = 945, .configured = true};
    model.peers[3].screens[0] = (kms_desktop_rect_t){0, 0, 1366, 10};
    kms_desktop_capture_reference(&model, UINT8_MAX);
    model.reference_gap = 20;
    kms_desktop_rect_t shifted[] = {{0, 1080, 1920, 1080}, {1920, 0, 3840, 2160}};
    report_screens(2, 90, shifted, 2);
    model.peers[3].ready = false;
    kms_desktop_master_tick(&model, 2, 77, true, request_time);
    CHECK(!(preview_kinds(2, 1) & (1u << KMS_D_ROUTE_REMEMBERED)));
    return 0;
}
static int refresh_join_tests(void)
{
    tolerance_desk(0);
    kms_desktop_capture_reference(&model, UINT8_MAX);
    kms_desktop_rect_t screens[] = {{0, 963, 1920, 1200}, {1920, 0, 3840, 2160}};
    report_screens(2, 9, screens, 2);
    uint8_t config[KMS_D_CONFIG_BYTES];
    CHECK(kms_desktop_config_encode(&model.config, config));
    for (unsigned refresh = 0; refresh < 2; ++refresh) {
        uint32_t token = 600 + refresh;
        kms_desktop_message_t q = {.op = KMS_D_CONFIG_BEGIN, .id = token, .session = model.peers[0].session};
        kms_put_u32(q.data, model.revision);
        kms_put_u32(q.data + 4, KMS_D_CONFIG_TOLERANCES);
        kms_put_u32(q.data + 8, 23);
        kms_put_u32(q.data + 12, 0);
        q.data[16] = (uint8_t)refresh;
        kms_desktop_master_request(&model, 1, 10, &q, request_time);
        CHECK(last.result == KMS_D_OK);
        for (unsigned offset = 0; offset < KMS_D_CONFIG_BYTES; offset += 32) {
            q = (kms_desktop_message_t){.op = KMS_D_CONFIG_CHUNK, .id = token + 10 + offset, .session = model.peers[0].session};
            kms_put_u32(q.data, token); kms_put_u32(q.data + 4, offset); memcpy(q.data + 8, config + offset, 32);
            kms_desktop_master_request(&model, 1, 10, &q, request_time);
            CHECK(last.result == KMS_D_OK);
        }
        request(1, KMS_D_CONFIG_COMMIT, token + 100, token, 0, 0, 0);
        CHECK(last.result == KMS_D_OK && model.config.gap_tolerance == 23 && model.config.overlap_tolerance == 0);
        CHECK(model.reference[1].screens[0].y == (refresh ? 963 : 960));
        CHECK(model.reference_gap == (refresh ? 23u : 0u));
    }
    return 0;
}
static int remembered_join_tests(void)
{
    inside_corner();
    model.config.stations[0].x = 277;
    model.config.stations[1].width = 5760; model.config.stations[1].height = 2160;
    model.config.stations[2] = (kms_desktop_station_t){.x = 5760, .configured = true};
    model.peers[2].screens[0] = (kms_desktop_rect_t){0, 0, 3840, 2160};
    kms_desktop_capture_reference(&model, UINT8_MAX);
    unsigned target; int32_t x, y;
    const int offsets[] = {-40, -1, 1, 120};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        int offset = offsets[i];
        kms_desktop_rect_t live[] = {{1920, 0, 3840, 2160}, {0, 960 + offset, 1920, 1200 - offset}};
        report_screens(2, (unsigned)(200 + offset), live, 2);
        kms_desktop_master_tick(&model, 2, 77, true, request_time);
        CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
        CHECK(kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 900, 960 + offset, &target, &x, &y));
        CHECK(target == 1 && x == 623 && y == 765);
        CHECK(kms_desktop_routes_cross(&routing, 1, KMS_D_DOWN, 623, 767, &target, &x, &y));
        CHECK(target == 2 && x == 900 && y == 962 + offset);
        CHECK(!kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 100, 960 + offset, &target, &x, &y));
        CHECK(!kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 4000, 0, &target, &x, &y));
        CHECK(kms_desktop_routes_cross(&routing, 2, KMS_D_RIGHT, 5759, 1000, &target, &x, &y) && target == 3);
    }
    request(2, KMS_D_PEER, 540, 2, KMS_D_PEER_JOIN_PREVIEW, 0, 0);
    CHECK(last.result == KMS_D_OK && last.data[4] == 1 && last.data[5] == KMS_D_ROUTE_REMEMBERED);
    CHECK(kms_get_u32(last.data + 16) == 277 && kms_get_u32(last.data + 20) == 1080);
    CHECK(kms_get_u32(last.data + 24) == 1643 && kms_get_u32(last.data + 28) == 1080);
    request(2, KMS_D_PEER, 541, 2, KMS_D_PEER_JOIN_PREVIEW | kms_get_u32(last.data + 32), 0, 0);
    CHECK(last.result == KMS_D_OK && last.data[4] == 0);
    model.config.stations[3] = (kms_desktop_station_t){.x = 277, .y = 960, .configured = true};
    model.peers[3].screens[0] = (kms_desktop_rect_t){0, 0, 1366, 120};
    kms_desktop_master_tick(&model, 2, 77, true, request_time);
    CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
    CHECK(kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 900, 1080, &target, &x, &y) && target == 4);
    // A screen midway across the gap is a barrier even when it touches neither side.
    model.config.stations[3].x = 750; model.config.stations[3].y = 980;
    model.peers[3].screens[0] = (kms_desktop_rect_t){0, 0, 300, 40};
    CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
    CHECK(!kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 900, 1080, &target, &x, &y) || target != 1);
    CHECK(kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 1100, 1080, &target, &x, &y) && target == 1);
    model.config.stations[3].configured = false;
    kms_desktop_rect_t external = {0, 0, 3840, 2160};
    report_screens(2, 330, &external, 1);
    kms_desktop_master_tick(&model, 2, 77, true, request_time);
    CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
    CHECK(!kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 900, 0, &target, &x, &y));
    kms_desktop_rect_t reopened[] = {{0, 1080, 1920, 1080}, {1920, 0, 3840, 2160}};
    report_screens(2, 331, reopened, 2);
    CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
    CHECK(kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 900, 1080, &target, &x, &y) && target == 1);
    // A desk edit saves the current geometry. An intentionally opened gap is
    // no longer a remembered join.
    model.config.stations[0].y -= 40;
    kms_desktop_capture_reference(&model, UINT8_MAX);
    ++model.topology_revision;
    CHECK(kms_desktop_routes_prepare(&routing, &model, model.availability));
    CHECK(!kms_desktop_routes_cross(&routing, 2, KMS_D_UP, 900, 1080, &target, &x, &y));
    return 0;
}
__declspec(dllexport) int kms_run_desktop_tests(void)
{
    checks = 0; fixture();
    kms_desktop_message_t q = {.op = KMS_D_HELLO, .id = 1, .session = 123}, decoded;
    uint8_t wire[KMS_D_BYTES]; CHECK(kms_desktop_encode(&q, wire)); CHECK(kms_desktop_decode(&decoded, wire, sizeof(wire)));
    CHECK(decoded.session == 123 && decoded.id == 1); CHECK(!kms_desktop_decode(&decoded, wire, 63));
    for (unsigned i = 0; i < sizeof(wire); ++i) { wire[i] ^= 1; CHECK(!kms_desktop_decode(&decoded, wire, sizeof(wire))); wire[i] ^= 1; }
    uint8_t config[KMS_D_CONFIG_BYTES]; kms_desktop_config_t c;
    model.config.stations[0].width = 3840; model.config.stations[0].height = 2160;
    CHECK(kms_desktop_config_encode(&model.config, config)); CHECK(kms_desktop_config_decode(&c, config, sizeof(config)));
    CHECK(c.stations[0].width == 3840 && c.stations[0].height == 2160);
    CHECK(c.stations[5].y == -100); config[10] ^= 1; CHECK(!kms_desktop_config_decode(&c, config, sizeof(config)));
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50); CHECK(last.result == 0 && model.handoff.target == 2);
    kms_desktop_master_tick(&model, 1, 77, true, 1000); CHECK(event.op == KMS_D_PREPARE && event.session == 11);
    request(2, KMS_D_READY, 2, model.handoff.id, 0, 0, 0); CHECK(selected == 2 && !model.handoff.active);
    fixture(); model.peers[1].enabled = false; request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50); CHECK(model.handoff.target == 3);
    fixture(); model.peers[1].seen = 0; request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    kms_desktop_master_tick(&model, 1, 77, true, 4000); CHECK(!model.handoff.active); // Source lease also expired.
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_DOWN, 50, 99); CHECK(model.handoff.target == 4);
    fixture(); model.peers[3].ready = false; request(1, KMS_D_CROSS, 1, 77, KMS_D_DOWN, 50, 99); CHECK(model.handoff.target == 5);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_UP, 50, 0); CHECK(model.handoff.target == 6 && model.handoff.landing_y == 97);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_LEFT, 0, 50); CHECK(model.handoff.target == 7 && model.handoff.landing_x == 97);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 2147483647, 50); CHECK(last.result == KMS_D_INVALID);
    fixture(); model.peers[0].screens[1] = (kms_desktop_rect_t){100, 0, 100, 100}; model.peers[0].count = 2;
    request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50); CHECK(last.result == KMS_D_INVALID && !model.handoff.active);
    // Exposed segment of an offset monitor is an edge; its internal seam is not.
    model.peers[0].screens[1].y = 50;
    request(1, KMS_D_CROSS, 2, 77, KMS_D_RIGHT, 99, 25); CHECK(last.result == KMS_D_OK && model.handoff.active);
    fixture(); model.config.stations[0].x = 250; model.config.stations[1].x = 200; model.config.stations[1].y = 100;
    model.peers[1].screens[1] = (kms_desktop_rect_t){100, 0, 100, 100}; model.peers[1].count = 2;
    request(1, KMS_D_CROSS, 1, 77, KMS_D_DOWN, 50, 99);
    CHECK(model.handoff.target == 2 && model.handoff.landing_x >= 100 && model.handoff.landing_x < 200 && model.handoff.landing_y == 2);
    // Fixed dock areas survive a laptop with a different desktop resolution.
    fixture(); model.config.stations[1].width = 100; model.config.stations[1].height = 100;
    model.peers[1].screens[0] = (kms_desktop_rect_t){0, 0, 200, 200};
    request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    CHECK(model.handoff.target == 2 && model.handoff.landing_y == 100);
    // Both visible contacts in an inside corner work despite overlapping station bounds.
    inside_corner(); request(1, KMS_D_CROSS, 1, 77, KMS_D_DOWN, 500, 767);
    CHECK(last.result == KMS_D_OK && model.handoff.target == 2);
    CHECK(model.handoff.landing_x == 1054 && model.handoff.landing_y == 962);
    inside_corner(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 1365, 300);
    CHECK(last.result == KMS_D_OK && model.handoff.target == 2);
    CHECK(model.handoff.landing_x == 1922 && model.handoff.landing_y == 492);
    // The reported arrangement: only the bottom touches; the right has a 277-unit gap.
    inside_corner(); model.config.stations[0].x = 277;
    request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 1365, 300);
    CHECK(last.result == KMS_D_OFFLINE && !model.handoff.active);
    request(1, KMS_D_CROSS, 2, 77, KMS_D_DOWN, 500, 767);
    CHECK(last.result == KMS_D_OK && model.handoff.target == 2);
    inside_corner(); model.config.stations[0].x = 277;
    kms_desktop_master_tick(&model, 2, 77, true, 1000);
    request(2, KMS_D_CROSS, 1, 77, KMS_D_LEFT, 1920, 492);
    CHECK(last.result == KMS_D_OFFLINE && !model.handoff.active);
    // With tolerance disabled, isolated awake groups still get a fallback.
    for (unsigned direction = KMS_D_LEFT; direction <= KMS_D_DOWN; ++direction) {
        inside_corner();
        model.config.gap_tolerance = model.config.overlap_tolerance = 0;
        model.config.stations[0].x = direction == KMS_D_LEFT ? 5761 : 553;
        model.config.stations[0].y = direction == KMS_D_UP ? 2161 : -769;
        int x = direction == KMS_D_LEFT ? 0 : direction == KMS_D_RIGHT ? 1365 : 500;
        int y = direction == KMS_D_UP ? 0 : direction == KMS_D_DOWN ? 767 : 300;
        if (direction <= KMS_D_RIGHT) model.config.stations[0].y = 192;
        else model.config.stations[0].x = 2000;
        request(1, KMS_D_CROSS, 1, 77, direction, x, y);
        if (direction == KMS_D_RIGHT) CHECK(last.result == KMS_D_OFFLINE && !model.handoff.active); // Bottom contact already connects them.
        else CHECK(last.result == KMS_D_OK && model.handoff.active && model.handoff.target == 2);
    }
    // Scaled areas and negative native origins still use exact geometric edges.
    for (unsigned direction = KMS_D_LEFT; direction <= KMS_D_DOWN; ++direction) {
        fixture();
        for (unsigned i = 2; i < KMS_SLOTS; ++i) model.config.stations[i].configured = false;
        model.peers[0].screens[0] = (kms_desktop_rect_t){-100, -50, 100, 100};
        model.peers[1].screens[0] = (kms_desktop_rect_t){-50, -100, 200, 150};
        model.config.stations[0].width = 333; model.config.stations[0].height = 151;
        model.config.stations[1].width = 277; model.config.stations[1].height = 123;
        model.config.stations[1].x = direction == KMS_D_LEFT ? -277 : direction == KMS_D_RIGHT ? 333 : 0;
        model.config.stations[1].y = direction == KMS_D_UP ? -123 : direction == KMS_D_DOWN ? 151 : 0;
        int x = direction == KMS_D_LEFT ? -100 : direction == KMS_D_RIGHT ? -1 : -50;
        int y = direction == KMS_D_UP ? -50 : direction == KMS_D_DOWN ? 49 : 0;
        request(1, KMS_D_CROSS, 1, 77, direction, x, y);
        CHECK(last.result == KMS_D_OK && model.handoff.target == 2);
        CHECK(model.handoff.landing_x >= -48 && model.handoff.landing_x <= 147);
        CHECK(model.handoff.landing_y >= -98 && model.handoff.landing_y <= 47);
        model.handoff.active = false;
        model.config.stations[1].x += (direction == KMS_D_RIGHT) - (direction == KMS_D_LEFT);
        model.config.stations[1].y += (direction == KMS_D_DOWN) - (direction == KMS_D_UP);
        request(1, KMS_D_CROSS, 2, 77, direction, x, y);
        CHECK(last.result == KMS_D_OK && model.handoff.active && model.handoff.target == 2);
    }
    inside_corner(); kms_desktop_master_tick(&model, 2, 77, true, 1000);
    request(2, KMS_D_CROSS, 1, 77, KMS_D_UP, 1054, 960);
    CHECK(last.result == KMS_D_OK && model.handoff.target == 1);
    CHECK(model.handoff.landing_x == 500 && model.handoff.landing_y == 765);
    inside_corner(); kms_desktop_master_tick(&model, 2, 77, true, 1000);
    request(2, KMS_D_CROSS, 1, 77, KMS_D_LEFT, 1920, 492);
    CHECK(last.result == KMS_D_OK && model.handoff.target == 1);
    CHECK(model.handoff.landing_x == 1363 && model.handoff.landing_y == 300);
    inside_corner(); kms_desktop_master_tick(&model, 2, 77, true, 1000);
    request(2, KMS_D_CROSS, 1, 77, KMS_D_LEFT, 1920, 1200);
    CHECK(last.result == KMS_D_INVALID && !model.handoff.active);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    kms_desktop_master_motion(&model, -1, 0); CHECK(!model.handoff.active);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    kms_desktop_master_tick(&model, 1, 78, true, 1001); CHECK(!model.handoff.active);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    kms_desktop_master_tick(&model, 1, 77, false, 1001); CHECK(!model.handoff.active);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    kms_desktop_master_tick(&model, 1, 77, true, 1301); CHECK(!model.handoff.active && !selected);
    selected = 0; // A late reply from the skipped neighbor cannot select it.
    request(2, KMS_D_READY, 2, 1, 0, 0, 0); CHECK(!selected && last.result == KMS_D_STALE);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    q = (kms_desktop_message_t){.op = KMS_D_HELLO, .id = 50, .session = 500};
    kms_desktop_master_request(&model, 2, 10, &q, 1001);
    CHECK(model.peers[1].count == 1 && !model.peers[1].geometry && !model.peers[1].ready && model.peers[1].session == 500);
    kms_desktop_master_tick(&model, 1, 77, true, 1002); CHECK(!model.handoff.active && !selected);
    fixture(); request(1, KMS_D_CONFIG_BEGIN, 10, 100, 0, 0, 0); CHECK(last.result == KMS_D_CONFLICT);
    request(1, KMS_D_CONFIG_BEGIN, 11, 101, 0, 0, 0); CHECK(last.result == 0 && model.upload.id == 11);
    CHECK(kms_desktop_config_encode(&model.config, config));
    for (unsigned offset = 0; offset < 96; offset += 32) {
        q = (kms_desktop_message_t){.op = KMS_D_CONFIG_CHUNK, .id = 12 + offset, .session = 10};
        kms_put_u32(q.data, 11); kms_put_u32(q.data + 4, offset); memcpy(q.data + 8, config + offset, 32);
        kms_desktop_master_request(&model, 1, 10, &q, 1100); CHECK(last.result == 0);
    }
    request(1, KMS_D_CONFIG_COMMIT, 100, 11, 0, 0, 0); CHECK(last.result == 0 && saves == 1 && model.revision == 102);
    request(1, KMS_D_CONFIG_COMMIT, 100, 11, 0, 0, 0); CHECK(last.result == 0 && saves == 1);
    fixture(); request(1, KMS_D_SCREENS_BEGIN, 1, 7, 2, 0, 0);
    request(1, KMS_D_SCREENS_COMMIT, 2, 7, 0, 0, 0); CHECK(last.result == KMS_D_INVALID && model.peers[0].geometry == 1);
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    request(3, KMS_D_POWER, 2, KMS_D_POWER_UNKNOWN, 0, 0, 0);
    CHECK(model.handoff.active && model.peers[2].ready);
    request(3, KMS_D_POWER, 3, KMS_D_POWER_SLEEP_OR_HIBERNATE, 0, 0, 0);
    CHECK(model.handoff.active && !model.peers[2].ready);
    fixture(); request(2, KMS_D_POWER, 1, KMS_D_POWER_SLEEP_OR_HIBERNATE, 0, 0, 0);
    CHECK(last.result == KMS_D_OK && !model.peers[1].ready);
    request(1, KMS_D_PEER, 2, 2, 0xffffffffu, 0, 0);
    CHECK(last.data[36] == KMS_D_POWER_SLEEP_OR_HIBERNATE && last.data[37] && last.data[38]);
    CHECK(!last.data[4] && !kms_get_u32(last.data + 40));
    q = (kms_desktop_message_t){.op = KMS_D_PEER, .id = 3, .session = 10};
    kms_put_u32(q.data, 2); kms_put_u32(q.data + 4, 0xffffffffu);
    kms_desktop_master_request(&model, 1, 10, &q, 4000);
    CHECK(!last.data[37] && last.data[36] == KMS_D_POWER_SLEEP_OR_HIBERNATE && kms_get_u32(last.data + 40) == 3000);
    request(2, KMS_D_POWER, 4, 99, 0, 0, 0); CHECK(last.result == KMS_D_INVALID);
    request(2, KMS_D_POWER, 5, KMS_D_POWER_AWAKE, 0, 0, 0);
    request(2, KMS_D_HEARTBEAT, 6, 3, 0, 0, 0);
    request(1, KMS_D_PEER, 7, 2, 0xffffffffu, 0, 0); CHECK(last.data[4] && last.data[36] == KMS_D_POWER_AWAKE);
    q = (kms_desktop_message_t){.op = KMS_D_HELLO, .id = 8, .session = 500};
    kms_desktop_master_request(&model, 2, 10, &q, 1001); CHECK(model.peers[1].power == KMS_D_POWER_UNKNOWN);
    // Stale/sleeping receivers remain manually selectable, never automatic destinations.
    sleeping_neighbor(); CHECK(!model.handoff.active && !selected);
    kms_desktop_master_tick(&model, 1, 77, true, 4001);
    CHECK(!selected && !model.handoff.active);
    request(2, KMS_D_READY, 2, 1, 0, 0, 0); CHECK(last.result == KMS_D_STALE && !selected);
    request(1, KMS_D_SELECT, 3, 2, 0, 0, 0); CHECK(selected == 2);
    sleeping_neighbor(); selection_ok = false;
    request(1, KMS_D_SELECT, 3, 2, 0, 0, 0); CHECK(!selected && last.result == KMS_D_OFFLINE);
    // An awake helper that never acknowledges cannot trigger an unconfirmed switch.
    fixture(); for (unsigned i = 2; i < KMS_SLOTS; ++i) model.config.stations[i].configured = false;
    request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    uint32_t timed_out = model.handoff.id;
    kms_desktop_master_tick(&model, 1, 77, true, 1299); CHECK(!selected && model.handoff.active);
    kms_desktop_master_tick(&model, 1, 77, true, 1300); CHECK(!selected && !model.handoff.active);
    request(2, KMS_D_READY, 2, timed_out, 0, 0, 0); CHECK(last.result == KMS_D_STALE && !selected);
    // Source cancellation and session/availability changes still gate selection.
    for (unsigned condition = 0; condition < 9; ++condition) {
        fixture(); for (unsigned i = 2; i < KMS_SLOTS; ++i) model.config.stations[i].configured = false;
        request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50); CHECK(model.handoff.active);
        if (condition == 0) kms_desktop_master_motion(&model, -1, 0);
        if (condition == 1) model.config.enabled = false;
        if (condition == 2) model.peers[0].session++;
        if (condition == 3) model.peers[0].seen = request_time - KMS_D_LEASE_MS;
        if (condition == 4) model.peers[1].session++;
        if (condition == 5) model.peers[1].enabled = false;
        if (condition == 6) model.peers[1].ready = false;
        kms_desktop_master_tick(&model, condition == 7 ? 3 : 1, 77, condition != 8, 1001);
        CHECK(!selected && !model.handoff.active);
    }
    // Sleep between CROSS and READY invalidates the queued route, even after resume.
    fixture(); request(1, KMS_D_CROSS, 1, 77, KMS_D_RIGHT, 99, 50);
    uint32_t stale = model.handoff.id;
    request(2, KMS_D_POWER, 2, KMS_D_POWER_SLEEP_OR_HIBERNATE, 0, 0, 0);
    request(2, KMS_D_POWER, 3, KMS_D_POWER_AWAKE, 0, 0, 0);
    request(2, KMS_D_HEARTBEAT, 4, 3, 0, 0, 0);
    request(2, KMS_D_READY, 5, stale, 0, 0, 0); CHECK(last.result == KMS_D_STALE && !selected);
    // The large right screen bypasses the sleeping middle PC to the small NUC.
    inside_corner(); request_time = 4000; model.peers[0].seen = model.peers[2].seen = request_time;
    model.peers[1].ready = false; model.peers[1].seen = 1;
    model.peers[2].screens[0] = (kms_desktop_rect_t){0, 0, 3840, 2160};
    model.config.stations[2] = (kms_desktop_station_t){.x = 5760, .y = 0, .configured = true};
    kms_desktop_master_tick(&model, 3, 77, true, request_time);
    request(3, KMS_D_CROSS, 1, 77, KMS_D_LEFT, 0, 1000);
    CHECK(model.handoff.active && model.handoff.target == 1 && !selected);
    request(1, KMS_D_READY, 2, model.handoff.id, 0, 0, 0); CHECK(selected == 1);
    // Allocation failure disables automatic routing; manual recovery stays available.
    fixture(); model.io.routes = NULL;
    request(1, KMS_D_CROSS, 9, 77, KMS_D_RIGHT, 99, 50);
    CHECK(last.result == KMS_D_OFFLINE && !model.handoff.active && !selected);
    request(1, KMS_D_SELECT, 10, 2, 0, 0, 0); CHECK(selected == 2);
    int failure = screen_persistence_tests();
    if (!failure) failure = offline_skipping_tests();
    if (!failure) failure = scaled_monitor_tests();
    if (!failure) failure = tolerance_tests();
    if (!failure) failure = refresh_join_tests();
    if (!failure) failure = remembered_join_tests();
    return failure ? failure : edge_departure_tests();
}
__declspec(dllexport) int kms_desktop_test_count(void) { return (int)checks; }
__declspec(dllexport) int kms_desktop_test_debug(unsigned field)
{
    return field == 0 ? (int)model.handoff.target : field == 1 ? model.handoff.landing_x :
        field == 2 ? model.handoff.landing_y : field == 3 ? (int)selected : (int)last.result;
}
__declspec(dllexport) void kms_desktop_sim_init(void) { fixture(); }
__declspec(dllexport) int kms_desktop_sim_exchange(unsigned slot, const uint8_t *in, uint8_t *out)
{
    kms_desktop_message_t message;
    if (!kms_desktop_decode(&message, in, KMS_D_BYTES)) return 0;
    kms_desktop_master_request(&model, slot, 10, &message, 1000);
    return kms_desktop_encode(&last, out);
}
