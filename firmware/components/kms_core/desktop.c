#include "kms_desktop.h"
#include "kms_desktop_routes.h"
#include <string.h>

bool kms_desktop_encode(const kms_desktop_message_t *m, uint8_t *b)
{
    if (!m || !b || m->op < KMS_D_HELLO || m->op > KMS_D_CHANNEL_RECOVERY_SET || m->result > KMS_D_CONFLICT) return false;
    memset(b, 0, KMS_D_BYTES); b[0] = 'K'; b[1] = KMS_D_VERSION; b[2] = m->op; b[3] = m->result;
    kms_put_u32(b + 4, m->id); kms_put_u32(b + 8, m->session); memcpy(b + 12, m->data, KMS_D_DATA);
    kms_put_u32(b + 60, kms_crc32(b, 60)); return true;
}
bool kms_desktop_decode(kms_desktop_message_t *m, const uint8_t *b, size_t n)
{
    if (!m || !b || n != KMS_D_BYTES || b[0] != 'K' || b[1] != KMS_D_VERSION ||
        b[2] < KMS_D_HELLO || b[2] > KMS_D_CHANNEL_RECOVERY_SET || b[3] > KMS_D_CONFLICT ||
        kms_get_u32(b + 60) != kms_crc32(b, 60)) return false;
    memset(m, 0, sizeof(*m)); m->op = b[2]; m->result = b[3];
    m->id = kms_get_u32(b + 4); m->session = kms_get_u32(b + 8); memcpy(m->data, b + 12, KMS_D_DATA); return true;
}
static bool coordinate(int32_t n) { return n >= -100000 && n <= 100000; }
static bool valid_screen(const kms_desktop_rect_t *r)
{
    return coordinate(r->x) && coordinate(r->y) && r->width >= 8 && r->height >= 8 &&
        r->width <= 32768 && r->height <= 32768;
}
bool kms_desktop_screens_encode(const kms_desktop_rect_t *screens, unsigned count, uint8_t *b)
{
    if (!screens || !b || !count || count > KMS_D_MONITORS) return false;
    memset(b, 0, KMS_D_SCREENS_BYTES); memcpy(b, "KDS1", 4); b[4] = (uint8_t)count;
    for (unsigned i = 0; i < count; ++i) {
        if (!valid_screen(&screens[i])) return false;
        uint8_t *r = b + 8 + i * 16;
        kms_put_u32(r, (uint32_t)screens[i].x); kms_put_u32(r + 4, (uint32_t)screens[i].y);
        kms_put_u32(r + 8, (uint32_t)screens[i].width); kms_put_u32(r + 12, (uint32_t)screens[i].height);
    }
    kms_put_u32(b + KMS_D_SCREENS_BYTES - 4, kms_crc32(b, KMS_D_SCREENS_BYTES - 4)); return true;
}
bool kms_desktop_screens_decode(kms_desktop_peer_t *p, const uint8_t *b, size_t n)
{
    if (!p || !b || n != KMS_D_SCREENS_BYTES || memcmp(b, "KDS1", 4) ||
        !b[4] || b[4] > KMS_D_MONITORS || b[5] || b[6] || b[7] ||
        kms_get_u32(b + n - 4) != kms_crc32(b, n - 4)) return false;
    kms_desktop_rect_t screens[KMS_D_MONITORS] = {0};
    for (unsigned i = 0; i < b[4]; ++i) {
        const uint8_t *r = b + 8 + i * 16;
        screens[i] = (kms_desktop_rect_t){(int32_t)kms_get_u32(r), (int32_t)kms_get_u32(r + 4),
            (int32_t)kms_get_u32(r + 8), (int32_t)kms_get_u32(r + 12)};
        if (!valid_screen(&screens[i])) return false;
    }
    // Saved geometry describes the desk, never a live helper session/readiness.
    p->count = b[4]; memcpy(p->screens, screens, sizeof(screens)); return true;
}
bool kms_desktop_config_encode(const kms_desktop_config_t *c, uint8_t *b)
{
    if (!c || !b || c->delay_ms > 2000 || c->corner > 64) return false;
    memset(b, 0, KMS_D_CONFIG_BYTES); memcpy(b, "KDL1", 4);
    b[4] = c->enabled; b[5] = c->corner; b[6] = (uint8_t)c->delay_ms; b[7] = (uint8_t)(c->delay_ms >> 8);
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        const kms_desktop_station_t *s = &c->stations[i];
        if (!coordinate(s->x) || !coordinate(s->y) || s->width > 32767 || s->height > 32767) return false;
        kms_put_u32(b + 8 + i * 12, (uint32_t)s->x); kms_put_u32(b + 12 + i * 12, (uint32_t)s->y);
        kms_put_u32(b + 16 + i * 12, (uint32_t)s->configured | (uint32_t)s->width << 1 | (uint32_t)s->height << 16);
    }
    kms_put_u32(b + 92, kms_crc32(b, 92)); return true;
}
bool kms_desktop_config_decode(kms_desktop_config_t *c, const uint8_t *b, size_t n)
{
    if (!c || !b || n != KMS_D_CONFIG_BYTES || memcmp(b, "KDL1", 4) || b[4] > 1 || b[5] > 64 ||
        kms_get_u32(b + 92) != kms_crc32(b, 92)) return false;
    kms_desktop_config_t v = {.enabled = b[4] != 0, .corner = b[5], .delay_ms = (uint16_t)(b[6] | b[7] << 8),
        .gap_tolerance = KMS_D_DEFAULT_TOLERANCE, .overlap_tolerance = KMS_D_DEFAULT_TOLERANCE};
    if (v.delay_ms > 2000) return false;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        kms_desktop_station_t *s = &v.stations[i]; s->x = (int32_t)kms_get_u32(b + 8 + i * 12);
        s->y = (int32_t)kms_get_u32(b + 12 + i * 12);
        uint32_t flags = kms_get_u32(b + 16 + i * 12);
        s->configured = (flags & 1) != 0; s->width = (uint16_t)((flags >> 1) & 32767); s->height = (uint16_t)((flags >> 16) & 32767);
        if (!coordinate(s->x) || !coordinate(s->y) || (flags & 0x80000000u)) return false;
    }
    *c = v; return true;
}
void kms_desktop_capture_reference(kms_desktop_master_t *m, uint8_t mask)
{
    for (unsigned i = 0; i < KMS_SLOTS; ++i) if (mask & (1u << i)) {
        m->reference[i].count = m->peers[i].count;
        memcpy(m->reference[i].screens, m->peers[i].screens, sizeof(m->reference[i].screens));
    }
}
bool kms_desktop_layout_encode(const kms_desktop_master_t *m, const kms_desktop_config_t *config,
                               uint8_t mask, uint8_t *out)
{
    if (!kms_desktop_config_encode(config, out)) return false;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        uint8_t *data = out + KMS_D_CONFIG_BYTES + i * KMS_D_SCREENS_BYTES;
        bool capture = (mask & (1u << i)) != 0;
        unsigned count = capture ? m->peers[i].count : m->reference[i].count;
        if (!count) memset(data, 0, KMS_D_SCREENS_BYTES);
        else if (!kms_desktop_screens_encode(capture ? m->peers[i].screens : m->reference[i].screens, count, data))
            return false;
    }
    uint8_t *tolerances = out + KMS_D_LEGACY_LAYOUT_BYTES - 4;
    bool replace = mask == (1u << KMS_SLOTS) - 1u;
    kms_put_u32(tolerances, config->gap_tolerance);
    kms_put_u32(tolerances + 4, config->overlap_tolerance);
    kms_put_u32(tolerances + 8, replace ? config->gap_tolerance : m->reference_gap);
    kms_put_u32(tolerances + 12, replace ? config->overlap_tolerance : m->reference_overlap);
    kms_put_u32(out + KMS_D_LAYOUT_BYTES - 4, kms_crc32(out, KMS_D_LAYOUT_BYTES - 4));
    return true;
}
bool kms_desktop_layout_decode(kms_desktop_master_t *m, const uint8_t *data, size_t size)
{
    kms_desktop_config_t config;
    if ((size != KMS_D_LAYOUT_BYTES && size != KMS_D_LEGACY_LAYOUT_BYTES) ||
        kms_get_u32(data + size - 4) != kms_crc32(data, size - 4) ||
        !kms_desktop_config_decode(&config, data, KMS_D_CONFIG_BYTES)) return false;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        kms_desktop_peer_t decoded;
        const uint8_t *screens = data + KMS_D_CONFIG_BYTES + i * KMS_D_SCREENS_BYTES;
        if (!screens[0]) { m->reference[i].count = 0; continue; }
        if (!kms_desktop_screens_decode(&decoded, screens, KMS_D_SCREENS_BYTES)) {
            memset(m->reference, 0, sizeof(m->reference));
            return false;
        }
        m->reference[i].count = decoded.count;
        memcpy(m->reference[i].screens, decoded.screens, sizeof(decoded.screens));
    }
    m->reference_gap = 0;
    m->reference_overlap = KMS_D_DEFAULT_TOLERANCE;
    if (size == KMS_D_LAYOUT_BYTES) {
        const uint8_t *tolerances = data + KMS_D_LEGACY_LAYOUT_BYTES - 4;
        config.gap_tolerance = kms_get_u32(tolerances);
        config.overlap_tolerance = kms_get_u32(tolerances + 4);
        m->reference_gap = kms_get_u32(tolerances + 8);
        m->reference_overlap = kms_get_u32(tolerances + 12);
    }
    m->config = config;
    return true;
}
static uint8_t reference_updates(const kms_desktop_master_t *m, const kms_desktop_config_t *config)
{
    uint8_t missing = 0;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        const kms_desktop_station_t *x = &m->config.stations[i], *y = &config->stations[i];
        if (!x->configured && !y->configured) continue;
        if (x->x != y->x || x->y != y->y || x->width != y->width || x->height != y->height ||
            x->configured != y->configured) return (1u << KMS_SLOTS) - 1u;
        if (!m->reference[i].count && m->peers[i].count) missing |= (uint8_t)(1u << i);
    }
    return missing;
}
void kms_desktop_master_init(kms_desktop_master_t *m, const kms_desktop_io_t *io, uint32_t revision)
{
    memset(m, 0, sizeof(*m)); m->io = *io; m->revision = revision ? revision : 1;
    if (m->io.routes) memset(m->io.routes, 0, sizeof(*m->io.routes));
    m->topology_revision = 1;
    m->config.enabled = true; m->config.delay_ms = 100; m->config.corner = 8;
    m->config.gap_tolerance = m->config.overlap_tolerance = KMS_D_DEFAULT_TOLERANCE;
    m->reference_overlap = KMS_D_DEFAULT_TOLERANCE;
}
static bool eligible(const kms_desktop_master_t *m, unsigned i, uint32_t now)
{
    const kms_desktop_peer_t *p = &m->peers[i];
    return m->config.stations[i].configured && p->session && p->enabled && p->geometry && p->count &&
        p->ready && p->power <= KMS_D_POWER_AWAKE && !kms_time_elapsed(now, p->seen, KMS_D_LEASE_MS) &&
        (!m->io.available || m->io.available(m->io.ctx, i + 1));
}
static void update_topology(kms_desktop_master_t *m, uint32_t now)
{
    uint8_t available = 0;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) if (eligible(m, i, now)) available |= (uint8_t)(1u << i);
    if (available != m->availability) { m->availability = available; ++m->topology_revision; }
}
static bool current_handoff(const kms_desktop_master_t *m)
{
    return m->handoff.topology == m->topology_revision && kms_desktop_routes_current(m->io.routes, m,
        m->availability & (uint8_t)~m->handoff.excluded);
}
static bool contains(const kms_desktop_rect_t *r, int32_t x, int32_t y)
{ return x >= r->x && y >= r->y && x < r->x + r->width && y < r->y + r->height; }
static bool source_edge(const kms_desktop_peer_t *p, int32_t x, int32_t y, unsigned d)
{
    bool inside = false; int32_t nx = x + (d == KMS_D_RIGHT) - (d == KMS_D_LEFT);
    int32_t ny = y + (d == KMS_D_DOWN) - (d == KMS_D_UP);
    for (unsigned i = 0; i < p->count; ++i) {
        inside |= contains(&p->screens[i], x, y);
        if (contains(&p->screens[i], nx, ny)) return false;
    }
    return inside;
}
static bool prepare_next(kms_desktop_master_t *m, uint32_t now)
{
    unsigned target; int32_t x, y;
    uint8_t mask = m->availability & (uint8_t)~m->handoff.excluded;
    if (!kms_desktop_routes_prepare(m->io.routes, m, mask) ||
        !kms_desktop_routes_cross(m->io.routes, m->handoff.source, m->handoff.direction,
            m->handoff.x, m->handoff.y, &target, &x, &y)) {
        m->handoff.active = false; return false;
    }
    m->handoff.active = true; m->handoff.target = target;
    m->handoff.destination_session = m->peers[target - 1].session;
    m->handoff.landing_x = x; m->handoff.landing_y = y;
    m->handoff.topology = m->topology_revision; m->handoff.at = now; m->handoff.sent = now - 100;
    if (!++m->sequence) ++m->sequence;
    m->handoff.id = m->sequence; return true;
}
void kms_desktop_master_motion(kms_desktop_master_t *m, int32_t x, int32_t y)
{
    if (m->handoff.active && ((m->handoff.direction == KMS_D_LEFT && x > 0) ||
        (m->handoff.direction == KMS_D_RIGHT && x < 0) || (m->handoff.direction == KMS_D_UP && y > 0) ||
        (m->handoff.direction == KMS_D_DOWN && y < 0))) m->handoff.active = false;
}
static bool select_handoff(kms_desktop_master_t *m, unsigned target)
{
    if (!m->io.select(m->io.ctx, target)) return false;
    m->edge_departure.source = m->handoff.source;
    m->edge_departure.session = m->handoff.session;
    m->edge_departure.request_id = m->handoff.request_id;
    m->edge_departure.target = target;
    m->edge_departure.generation = m->handoff.generation + 1;
    return true;
}
void kms_desktop_master_tick(kms_desktop_master_t *m, unsigned selected, uint32_t generation, bool idle, uint32_t now)
{
    m->selected = selected; m->generation = generation; m->idle = idle;
    update_topology(m, now);
    if (!m->handoff.active) return;
    if (!current_handoff(m) || !m->config.enabled || selected != m->handoff.source || generation != m->handoff.generation || !idle ||
        !eligible(m, selected - 1, now) || m->peers[selected - 1].session != m->handoff.session) {
        m->handoff.active = false; return;
    }
    if (!eligible(m, m->handoff.target - 1, now) ||
        m->peers[m->handoff.target - 1].session != m->handoff.destination_session) {
        m->handoff.active = false; return;
    }
    if (kms_time_elapsed(now, m->handoff.at, KMS_D_HANDOFF_MS)) {
        m->handoff.excluded |= (uint8_t)(1u << (m->handoff.target - 1));
        if (!prepare_next(m, now)) return;
    }
    if (kms_time_elapsed(now, m->handoff.sent, 60)) {
        kms_desktop_message_t event = {.op = KMS_D_PREPARE, .id = m->handoff.id, .session = m->handoff.destination_session};
        kms_put_u32(event.data, (uint32_t)m->handoff.landing_x); kms_put_u32(event.data + 4, (uint32_t)m->handoff.landing_y);
        kms_put_u32(event.data + 8, m->peers[m->handoff.target - 1].geometry);
        if (m->io.send(m->io.ctx, m->handoff.target, &event)) m->handoff.sent = now;
    }
}
void kms_desktop_master_request(kms_desktop_master_t *m, unsigned slot, uint32_t boot,
                                const kms_desktop_message_t *q, uint32_t now)
{
    if (!slot || slot > KMS_SLOTS || !q->id || !q->session || q->result) return;
    update_topology(m, now);
    kms_desktop_peer_t *p = &m->peers[slot - 1];
    kms_desktop_message_t r = {.op = q->op, .id = q->id, .session = q->session};
    uint32_t a = kms_get_u32(q->data), b = kms_get_u32(q->data + 4);
    if (q->op == KMS_D_HELLO) {
        if (p->session != q->session || p->boot != boot) {
            // Keep the last screen shape while a restarted/new host reports its desktop.
            p->session = q->session; p->boot = boot; p->geometry = p->staging_geometry = p->mask = 0;
            p->enabled = p->ready = false; p->staging_count = 0; ++m->topology_revision;
            p->power = KMS_D_POWER_UNKNOWN; p->power_seen = 0;
        }
        p->seen = now;
    } else if (p->session != q->session || p->boot != boot) r.result = KMS_D_STALE;
    else switch (q->op) {
    case KMS_D_POWER:
        if (a > KMS_D_POWER_SHUTDOWN) { r.result = KMS_D_INVALID; break; }
        if (p->power != a) ++m->topology_revision;
        p->power = (uint8_t)a; p->power_seen = now;
        if (a == KMS_D_POWER_SLEEP_OR_HIBERNATE || a == KMS_D_POWER_SHUTDOWN) {
            p->ready = false;
            if (m->handoff.source == slot || m->handoff.target == slot) m->handoff.active = false;
        }
        break;
    case KMS_D_HEARTBEAT:
        if (p->enabled != ((a & 1) != 0) || p->ready != ((a & 2) != 0)) ++m->topology_revision;
        p->enabled = (a & 1) != 0; p->ready = (a & 2) != 0; p->seen = now; break;
    case KMS_D_SCREENS_BEGIN:
        if (!a || !b || b > KMS_D_MONITORS) { r.result = KMS_D_INVALID; break; }
        if (p->staging_geometry == a && p->staging_count != b) { r.result = KMS_D_INVALID; break; }
        if (p->staging_geometry != a) { p->mask = 0; p->staging_geometry = a; p->staging_count = (uint8_t)b; }
        break;
    case KMS_D_SCREEN: {
        kms_desktop_rect_t rect = {(int32_t)kms_get_u32(q->data + 8), (int32_t)kms_get_u32(q->data + 12),
            (int32_t)kms_get_u32(q->data + 16), (int32_t)kms_get_u32(q->data + 20)};
        if (!a || a != p->staging_geometry || b >= p->staging_count || !valid_screen(&rect)) r.result = KMS_D_INVALID;
        else { p->staging[b] = rect; p->mask |= 1u << b; }
        break; }
    case KMS_D_SCREENS_COMMIT:
        if (!a || a != p->staging_geometry || p->mask != (1u << p->staging_count) - 1) r.result = KMS_D_INVALID;
        else if (p->geometry != a) {
            bool changed = p->count != p->staging_count ||
                memcmp(p->screens, p->staging, p->staging_count * sizeof(p->screens[0]));
            if (changed && m->io.save_screens) {
                uint8_t data[KMS_D_SCREENS_BYTES];
                if (!kms_desktop_screens_encode(p->staging, p->staging_count, data) ||
                    !m->io.save_screens(m->io.ctx, slot, data)) { r.result = KMS_D_STORAGE; break; }
            }
            memcpy(p->screens, p->staging, sizeof(p->screens)); p->count = p->staging_count;
            p->geometry = a; m->handoff.active = false;
            ++m->topology_revision;
        }
        break;
    case KMS_D_CONFIG_READ: {
        uint8_t blob[KMS_D_CONFIG_BYTES];
        if (a > KMS_D_CONFIG_BYTES - 32) r.result = KMS_D_INVALID;
        else {
            kms_desktop_config_encode(&m->config, blob);
            memcpy(r.data + 8, blob + a, 32); kms_put_u32(r.data + 4, a);
            if (b == KMS_D_CONFIG_TOLERANCES) {
                r.data[7] = 1;
                kms_put_u32(r.data + 40, m->config.gap_tolerance);
                kms_put_u32(r.data + 44, m->config.overlap_tolerance);
            }
        }
        break; }
    case KMS_D_CONFIG_BEGIN:
        if (a != m->revision) r.result = KMS_D_CONFLICT;
        else if (m->upload.slot && !kms_time_elapsed(now, m->upload.at, 5000) &&
                 (m->upload.slot != slot || m->upload.session != q->session)) r.result = KMS_D_BUSY;
        else if (m->upload.id != q->id || m->upload.session != q->session) {
            memset(&m->upload, 0, sizeof(m->upload)); m->upload.slot = slot; m->upload.session = q->session;
            m->upload.id = q->id; m->upload.revision = a; m->upload.at = now;
            m->upload.gap_tolerance = b == KMS_D_CONFIG_TOLERANCES ? kms_get_u32(q->data + 8) : m->config.gap_tolerance;
            m->upload.overlap_tolerance = b == KMS_D_CONFIG_TOLERANCES ? kms_get_u32(q->data + 12) : m->config.overlap_tolerance;
            m->upload.refresh_joins = b == KMS_D_CONFIG_TOLERANCES && q->data[16] == 1;
        }
        kms_put_u32(r.data + 4, m->upload.id);
        kms_put_u32(r.data + 8, KMS_D_CONFIG_TOLERANCES);
        break;
    case KMS_D_CONFIG_CHUNK:
        if (m->upload.slot != slot || m->upload.session != q->session || a != m->upload.id || b > 64 || b % 32 ||
            kms_time_elapsed(now, m->upload.at, 5000)) r.result = KMS_D_STALE;
        else { memcpy(m->upload.data + b, q->data + 8, 32); m->upload.mask |= (uint8_t)(1u << (b / 32)); }
        break;
    case KMS_D_CONFIG_COMMIT: {
        kms_desktop_config_t c;
        if (m->last_source == slot && m->last_session == q->session && m->last_id == q->id) { r.result = m->last_result; break; }
        if (m->upload.slot != slot || m->upload.session != q->session || a != m->upload.id || m->upload.mask != 7 ||
            kms_time_elapsed(now, m->upload.at, 5000)) r.result = KMS_D_STALE;
        else if (m->revision != m->upload.revision) r.result = KMS_D_CONFLICT;
        else if (!kms_desktop_config_decode(&c, m->upload.data, sizeof(m->upload.data))) r.result = KMS_D_INVALID;
        else {
            c.gap_tolerance = m->upload.gap_tolerance;
            c.overlap_tolerance = m->upload.overlap_tolerance;
            uint8_t mask = m->upload.refresh_joins ? (1u << KMS_SLOTS) - 1u : reference_updates(m, &c);
            if (!m->io.save(m->io.ctx, &c, m, mask)) r.result = KMS_D_STORAGE;
            else {
                kms_desktop_capture_reference(m, mask);
                if (mask == (1u << KMS_SLOTS) - 1u) {
                    m->reference_gap = c.gap_tolerance;
                    m->reference_overlap = c.overlap_tolerance;
                }
                m->config = c;
                if (!++m->revision) ++m->revision;
                ++m->topology_revision;
                m->handoff.active = false;
            }
        }
        m->last_source = slot; m->last_session = q->session; m->last_id = q->id; m->last_result = r.result; m->upload.slot = 0;
        break; }
    case KMS_D_PEER:
        if (!a || a > KMS_SLOTS) r.result = KMS_D_INVALID;
        else if (b != UINT32_MAX && (b & KMS_D_PEER_JOIN_PREVIEW)) {
            kms_put_u32(r.data + 44, m->topology_revision);
            if (m->handoff.active) { r.result = KMS_D_BUSY; break; }
            if (m->config.enabled && kms_desktop_routes_prepare(m->io.routes, m, m->availability)) {
                kms_desktop_route_preview_t preview;
                bool all = (b & KMS_D_PEER_ALL_ROUTES) != 0;
                uint32_t cursor = b & ~(KMS_D_PEER_JOIN_PREVIEW | KMS_D_PEER_ALL_ROUTES);
                if (kms_desktop_routes_preview(m->io.routes, a, cursor, all, &preview)) {
                    r.data[4] = 1;
                    r.data[5] = preview.kind; r.data[6] = preview.target; r.data[7] = preview.direction;
                    for (unsigned i = 0; i < 4; ++i) kms_put_u32(r.data + 16 + i * 4, (uint32_t)preview.line[i]);
                    kms_put_u32(r.data + 32, preview.next);
                }
            }
        } else { const kms_desktop_peer_t *v = &m->peers[a - 1];
            r.data[4] = eligible(m, a - 1, now); r.data[5] = v->count; r.data[6] = v->enabled; r.data[7] = v->ready;
            kms_put_u32(r.data + 8, v->session); kms_put_u32(r.data + 12, now - v->seen);
            kms_put_u32(r.data + 32, v->geometry);
            r.data[36] = v->power;
            r.data[37] = v->power != KMS_D_POWER_UNKNOWN && !kms_time_elapsed(now, v->power_seen, KMS_D_LEASE_MS);
            r.data[38] = 1; // Power reporting supported by this master.
            if (kms_get_u32(q->data + 8) == KMS_D_PEER_ALL_ROUTES)
                r.data[39] = 2u | (m->config.enabled && r.data[4]);
            else r.data[39] = m->config.enabled && m->reference[a - 1].count && r.data[4];
            kms_put_u32(r.data + 44, m->topology_revision);
            kms_put_u32(r.data + 40, v->power == KMS_D_POWER_UNKNOWN ? UINT32_MAX : now - v->power_seen);
            if (b < v->count) { const kms_desktop_rect_t *s = &v->screens[b];
                kms_put_u32(r.data + 16, (uint32_t)s->x); kms_put_u32(r.data + 20, (uint32_t)s->y);
                kms_put_u32(r.data + 24, (uint32_t)s->width); kms_put_u32(r.data + 28, (uint32_t)s->height); }
        } break;
    case KMS_D_CROSS: {
        int32_t x = (int32_t)kms_get_u32(q->data + 8), y = (int32_t)kms_get_u32(q->data + 12);
        if (m->handoff.active && m->handoff.source == slot && m->handoff.session == q->session) break;
        if (!m->config.enabled || !m->idle || m->selected != slot || a != m->generation ||
            !eligible(m, slot - 1, now) || kms_get_u32(q->data + 16) != p->geometry) r.result = KMS_D_OFFLINE;
        else if (b > KMS_D_DOWN || !coordinate(x) || !coordinate(y) || !source_edge(p, x, y, b)) r.result = KMS_D_INVALID;
        else if (m->handoff.active) r.result = KMS_D_BUSY;
        else {
            memset(&m->handoff, 0, sizeof(m->handoff)); m->handoff.active = true;
            m->handoff.request_id = q->id;
            m->handoff.source = slot; m->handoff.session = q->session; m->handoff.generation = a; m->handoff.direction = (uint8_t)b;
            m->handoff.x = x; m->handoff.y = y;
            if (!prepare_next(m, now)) r.result = KMS_D_OFFLINE;
        } break; }
    case KMS_D_READY:
        if (!m->handoff.active || slot != m->handoff.target || q->session != m->handoff.destination_session ||
            a != m->handoff.id || !current_handoff(m) || kms_time_elapsed(now, m->handoff.at, KMS_D_HANDOFF_MS)) {
            if (!current_handoff(m)) m->handoff.active = false;
            r.result = KMS_D_STALE;
        }
        else if (b || !m->idle || m->selected != m->handoff.source || m->generation != m->handoff.generation ||
                 !eligible(m, slot - 1, now) || !eligible(m, m->handoff.source - 1, now)) {
            m->handoff.excluded |= (uint8_t)(1u << (slot - 1)); prepare_next(m, now); r.result = KMS_D_OFFLINE;
        } else { unsigned target = m->handoff.target; m->handoff.active = false;
            if (!select_handoff(m, target)) r.result = KMS_D_BUSY; }
        break;
    case KMS_D_SELECT:
        m->handoff.active = false;
        m->edge_departure.request_id = 0;
        if (!a || a > KMS_SLOTS || !m->io.select(m->io.ctx, a)) r.result = KMS_D_OFFLINE;
        break;
    default: r.result = KMS_D_UNSUPPORTED; break;
    }
    kms_put_u32(r.data, m->revision);
    if (q->op == KMS_D_HELLO || q->op == KMS_D_HEARTBEAT) {
        kms_put_u32(r.data + 4, m->selected); kms_put_u32(r.data + 8, m->generation);
        kms_put_u32(r.data + 12, m->topology_revision);
        if (slot == m->edge_departure.source && q->session == m->edge_departure.session &&
            m->selected == m->edge_departure.target && m->generation == m->edge_departure.generation)
            kms_put_u32(r.data + 16, m->edge_departure.request_id);
    }
    m->io.send(m->io.ctx, slot, &r);
}
