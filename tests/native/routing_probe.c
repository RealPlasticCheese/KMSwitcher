// Read-only audit adapter for the REAL production handoff state machine.
// It is linked into the desktop test library, never into device firmware.
#include "kms_desktop.h"
#include "kms_desktop_routes.h"
#include <string.h>
#include "../../firmware/components/kms_core/desktop_routes.c"

static kms_desktop_master_t probe;
static kms_desktop_routes_t routing;
static unsigned probe_selected, probe_reachable;
static uint32_t probe_request;
static int32_t probe_landing_x, probe_landing_y;
static bool probe_send(void *ctx, unsigned slot, const kms_desktop_message_t *message)
{ (void)ctx; (void)slot; (void)message; return true; }
static bool probe_available(void *ctx, unsigned slot)
{ (void)ctx; return (probe_reachable & (1u << (slot - 1))) != 0; }
static bool probe_select(void *ctx, unsigned slot)
{ (void)ctx; if (!probe_available(NULL, slot)) return false; probe_selected = slot; return true; }

// Each record is [slot, x, y, width, height], in global desktop pixels.
__declspec(dllexport) int kms_routing_probe_init(unsigned count, const int32_t *records,
                                               unsigned awake, unsigned sleeping, unsigned reachable)
{
    if (!records || count > KMS_SLOTS * KMS_D_MONITORS) return 0;
    const kms_desktop_io_t io = {.send = probe_send, .select = probe_select, .available = probe_available, .routes = &routing};
    kms_desktop_master_init(&probe, &io, 1);
    // This differential audit is against the exact-contact reference model.
    // The configurable bridging policy has its own production fixtures.
    probe.config.gap_tolerance = probe.config.overlap_tolerance = 0;
    probe_reachable = reachable; probe_selected = 0; probe_request = 0;
    for (unsigned row = 0; row < count; ++row) {
        const int32_t *r = records + row * 5;
        if (r[0] < 1 || r[0] > KMS_SLOTS || r[3] < 8 || r[4] < 8) return 0;
        unsigned index = (unsigned)r[0] - 1;
        kms_desktop_peer_t *p = &probe.peers[index];
        kms_desktop_station_t *s = &probe.config.stations[index];
        if (p->count == KMS_D_MONITORS) return 0;
        if (!p->count || r[1] < s->x) s->x = r[1];
        if (!p->count || r[2] < s->y) s->y = r[2];
        p->screens[p->count++] = (kms_desktop_rect_t){r[1], r[2], r[3], r[4]};
        p->session = index + 10; p->boot = 10; p->geometry = 1; p->enabled = true;
        p->ready = (awake & (1u << index)) != 0;
        p->seen = p->ready ? 4000 : 1;
        p->power = p->ready ? KMS_D_POWER_AWAKE :
            (sleeping & (1u << index)) ? KMS_D_POWER_SLEEP_OR_HIBERNATE : KMS_D_POWER_SHUTDOWN;
        s->configured = true;
    }
    return 1;
}

__declspec(dllexport) unsigned kms_routing_probe_begin(unsigned source, unsigned direction, int32_t x, int32_t y)
{
    if (!source || source > KMS_SLOTS || direction > KMS_D_DOWN) return 0;
    probe_selected = 0; probe.handoff.active = false;
    memset(&probe.edge_departure, 0, sizeof(probe.edge_departure));
    kms_desktop_master_tick(&probe, source, 77, true, 4000);
    kms_desktop_message_t q = {.op = KMS_D_CROSS, .id = ++probe_request,
        .session = probe.peers[source - 1].session};
    kms_put_u32(q.data, 77); kms_put_u32(q.data + 4, direction);
    kms_put_u32(q.data + 8, (uint32_t)x); kms_put_u32(q.data + 12, (uint32_t)y); kms_put_u32(q.data + 16, 1);
    kms_desktop_master_request(&probe, source, 10, &q, 4000);
    probe_landing_x = probe.handoff.landing_x; probe_landing_y = probe.handoff.landing_y;
    kms_desktop_master_tick(&probe, source, 77, true, 4001);
    return probe.handoff.active ? probe.handoff.id : 0;
}
__declspec(dllexport) unsigned kms_routing_probe_finish(unsigned target, uint32_t id, uint32_t session)
{
    kms_desktop_message_t q = {.op = KMS_D_READY, .id = ++probe_request, .session = session};
    kms_put_u32(q.data, id);
    kms_desktop_master_request(&probe, target, 10, &q, 4001);
    return probe_selected;
}
__declspec(dllexport) unsigned kms_routing_probe_cross(unsigned source, unsigned direction, int32_t x, int32_t y)
{
    uint32_t id = kms_routing_probe_begin(source, direction, x, y);
    return id ? kms_routing_probe_finish(probe.handoff.target, id, probe.handoff.destination_session) : 0;
}
// Mutate a pending handoff's world without resetting its sequence or workspace.
// Power/session/readiness changes use the real protocol; others emulate live link/config changes.
__declspec(dllexport) void kms_routing_probe_change(unsigned slot, unsigned cause)
{
    kms_desktop_peer_t *p = &probe.peers[slot - 1];
    kms_desktop_message_t q = {.op = KMS_D_POWER, .id = ++probe_request, .session = p->session};
    if (cause <= 1 || cause == 9) {
        kms_put_u32(q.data, cause == 1 ? KMS_D_POWER_SHUTDOWN : KMS_D_POWER_SLEEP_OR_HIBERNATE);
        kms_desktop_master_request(&probe, slot, 10, &q, 4001);
        if (cause == 9) {
            q.id = ++probe_request; kms_put_u32(q.data, KMS_D_POWER_AWAKE);
            kms_desktop_master_request(&probe, slot, 10, &q, 4001);
            q.id = ++probe_request; q.op = KMS_D_HEARTBEAT; kms_put_u32(q.data, 3);
            kms_desktop_master_request(&probe, slot, 10, &q, 4001);
        }
    } else if (cause <= 3) {
        q.op = KMS_D_HEARTBEAT; kms_put_u32(q.data, cause == 2 ? 1 : 0);
        kms_desktop_master_request(&probe, slot, 10, &q, 4001);
    } else if (cause == 4) probe_reachable &= ~(1u << (slot - 1));
    else if (cause == 5) p->seen = 1;
    else if (cause == 6) {
        q.op = KMS_D_HELLO; ++q.session; kms_desktop_master_request(&probe, slot, 10, &q, 4001);
    } else if (cause == 7) { ++p->screens[0].x; ++p->geometry; }
    else if (cause == 8) ++probe.config.stations[slot - 1].x;
}

// Rich fixture states exercise the production eligibility gates independently.
__declspec(dllexport) int kms_routing_probe_state(unsigned count, const int32_t *records,
                                                const int32_t *states, unsigned corner, uint32_t revision)
{
    if (corner > 64 || !kms_routing_probe_init(count, records, 0, 0, 0)) return 0;
    probe.config.corner = (uint8_t)corner; probe.topology_revision = revision + 1;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        kms_desktop_peer_t *p = &probe.peers[i]; const int32_t *s = states + 7 * i;
        p->power = (uint8_t)s[0]; p->seen = s[1] ? 4000 : 1; p->ready = s[2] != 0;
        if (s[3]) probe_reachable |= 1u << i;
        p->enabled = s[4] != 0; p->session = (uint32_t)s[5]; p->boot = (uint32_t)s[6];
    }
    kms_desktop_master_tick(&probe, 0, 77, true, 4000);
    return kms_desktop_routes_prepare(&routing, &probe, probe.availability) ? 1 : -1;
}
__declspec(dllexport) unsigned kms_routing_probe_nodes(void) { return routing.count; }
__declspec(dllexport) int kms_routing_probe_node(unsigned index, int32_t *out)
{
    if (index >= routing.count) return 0;
    const kms_route_node_t *node = &routing.nodes[index];
    out[0] = node->slot + 1; out[1] = node->ordinal; out[2] = node->monitors;
    out[3] = active(&routing, index); return 1;
}
__declspec(dllexport) unsigned kms_routing_probe_edges(unsigned from, unsigned to, unsigned d, int32_t *out)
{
    if (from >= routing.count || to >= routing.count || !active(&routing, from) || !active(&routing, to) || d > 3 || !(routing.links[from][to] & (1u << d))) return 0;
    unsigned n = portal_edges(&routing, from, to, d);
    for (unsigned i = 0; i < n; ++i) {
        const kms_route_edge_t *e = &routing.portal[i];
        out[4 * i] = (int32_t)d; out[4 * i + 1] = e->fixed;
        out[4 * i + 2] = e->start; out[4 * i + 3] = e->end;
    }
    return n;
}
__declspec(dllexport) int kms_routing_probe_virtual(unsigned from, unsigned to, unsigned d)
{ return !(routing.links[from][to] & (1u << (d + 4))); }
__declspec(dllexport) unsigned kms_routing_probe_links(unsigned from, unsigned to)
{ return active(&routing, from) && active(&routing, to) ? routing.links[from][to] : 0; }
__declspec(dllexport) void kms_routing_probe_landing(int32_t *out)
{ out[0] = probe_landing_x; out[1] = probe_landing_y; }
__declspec(dllexport) unsigned kms_routing_probe_bytes(void) { return sizeof(routing); }
__declspec(dllexport) unsigned kms_routing_probe_builds(void) { return routing.builds; }
