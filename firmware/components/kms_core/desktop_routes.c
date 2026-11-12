#include "kms_desktop_routes.h"
#include <string.h>

_Static_assert(sizeof(kms_desktop_routes_t) <= KMS_ROUTE_BUDGET, "Routing workspace exceeds its fixed RAM budget");

static int32_t minimum(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t maximum(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t right(const kms_desktop_rect_t *r) { return r->x + r->width; }
static int32_t bottom(const kms_desktop_rect_t *r) { return r->y + r->height; }
static bool contains(const kms_desktop_rect_t *r, int32_t x, int32_t y)
{ return x >= r->x && x < right(r) && y >= r->y && y < bottom(r); }
static bool touches(const kms_desktop_rect_t *a, const kms_desktop_rect_t *b)
{
    int32_t dx = minimum(right(a), right(b)) - maximum(a->x, b->x);
    int32_t dy = minimum(bottom(a), bottom(b)) - maximum(a->y, b->y);
    return dx >= 0 && dy >= 0 && (dx > 0 || dy > 0);
}
static bool rect_before(const kms_desktop_rect_t *a, const kms_desktop_rect_t *b)
{
    if (a->x != b->x) return a->x < b->x;
    if (a->y != b->y) return a->y < b->y;
    if (a->width != b->width) return a->width < b->width;
    return a->height < b->height;
}
static kms_desktop_rect_t bounds(const kms_desktop_rect_t *screens, unsigned count, uint16_t mask)
{
    kms_desktop_rect_t result = {0}; bool first = true;
    for (unsigned i = 0; i < count; ++i) if (mask & (1u << i)) {
        const kms_desktop_rect_t *r = &screens[i];
        if (first) { result = *r; first = false; continue; }
        int32_t x = minimum(result.x, r->x), y = minimum(result.y, r->y);
        result.width = maximum(right(&result), right(r)) - x;
        result.height = maximum(bottom(&result), bottom(r)) - y;
        result.x = x; result.y = y;
    }
    return result;
}
static int32_t scale(int32_t value, int32_t from, int32_t to)
{ return (int32_t)((int64_t)value * to / from); }
static kms_desktop_rect_t place_screen(const kms_desktop_rect_t *r, const kms_desktop_rect_t *box,
                                       const kms_desktop_station_t *station)
{
    int32_t width = station->width ? station->width : box->width;
    int32_t height = station->height ? station->height : box->height;
    int32_t x = scale(r->x - box->x, box->width, width);
    int32_t y = scale(r->y - box->y, box->height, height);
    return (kms_desktop_rect_t){station->x + x, station->y + y,
        scale(right(r) - box->x, box->width, width) - x,
        scale(bottom(r) - box->y, box->height, height) - y};
}
static void match_roles(kms_desktop_routes_t *p, const kms_desktop_master_t *m, unsigned slot)
{
    const kms_desktop_reference_t *reference = &m->reference[slot];
    p->reference_counts[slot] = reference->count;
    memset(p->roles[slot], UINT8_MAX, sizeof(p->roles[slot]));
    if (!reference->count) return;
    kms_desktop_rect_t box = bounds(reference->screens, reference->count, UINT16_MAX);
    uint8_t matches[KMS_D_MONITORS] = {0};
    for (unsigned j = 0; j < reference->count; ++j)
        p->reference[slot][j] = place_screen(&reference->screens[j], &box, &m->config.stations[slot]);
    // Display numbers and hardware IDs can change. A role requires mutual
    // containment of the old and new centres and a one-to-one match.
    for (unsigned i = 0; i < p->counts[slot]; ++i) {
        const kms_desktop_rect_t *live = &p->world[slot][i];
        unsigned candidate = UINT8_MAX, count = 0;
        for (unsigned j = 0; j < reference->count; ++j) {
            const kms_desktop_rect_t *saved = &p->reference[slot][j];
            if (contains(saved, live->x + live->width / 2, live->y + live->height / 2) &&
                contains(live, saved->x + saved->width / 2, saved->y + saved->height / 2)) {
                candidate = j; ++count; ++matches[j];
            }
        }
        if (count == 1) p->roles[slot][i] = (uint8_t)candidate;
    }
    for (unsigned i = 0; i < p->counts[slot]; ++i) {
        unsigned role = p->roles[slot][i];
        if (role != UINT8_MAX && matches[role] != 1) p->roles[slot][i] = UINT8_MAX;
    }
}
static void station_nodes(kms_desktop_routes_t *p, const kms_desktop_master_t *m, unsigned slot)
{
    const kms_desktop_peer_t *peer = &m->peers[slot];
    const kms_desktop_station_t *station = &m->config.stations[slot];
    if (!station->configured || !peer->count || peer->count > KMS_D_MONITORS) return;
    unsigned count = peer->count; p->counts[slot] = (uint8_t)count;
    memcpy(p->local[slot], peer->screens, count * sizeof(peer->screens[0]));
    kms_desktop_rect_t box = bounds(peer->screens, count, UINT16_MAX);
    p->desktop[slot] = box;
    for (unsigned i = 0; i < count; ++i) {
        p->order[slot][i] = (uint8_t)i;
        p->world[slot][i] = place_screen(&peer->screens[i], &box, station);
        for (unsigned j = i; j && rect_before(&peer->screens[p->order[slot][j]], &peer->screens[p->order[slot][j - 1]]); --j) {
            uint8_t previous = p->order[slot][j - 1]; p->order[slot][j - 1] = p->order[slot][j]; p->order[slot][j] = previous;
        }
    }
    match_roles(p, m, slot);
    uint16_t assigned = 0; unsigned ordinal = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned first = p->order[slot][i]; if (assigned & (1u << first)) continue;
        uint16_t group = (uint16_t)(1u << first), previous;
        do {
            previous = group;
            for (unsigned a = 0; a < count; ++a) if (!(group & (1u << a)))
                for (unsigned b = 0; b < count; ++b) if ((group & (1u << b)) && touches(&peer->screens[a], &peer->screens[b])) {
                    group |= (uint16_t)(1u << a); break;
                }
        } while (group != previous);
        assigned |= group;
        kms_route_node_t *node = &p->nodes[p->count++];
        node->slot = (uint8_t)slot; node->ordinal = (uint8_t)ordinal++; node->monitors = group;
        node->bounds = bounds(p->world[slot], count, group);
    }
}
static bool active(const kms_desktop_routes_t *p, unsigned node)
{ return (p->active & (1u << p->nodes[node].slot)) != 0; }
static unsigned root(kms_desktop_routes_t *p, unsigned node)
{
    while (p->parent[node] != node) { p->parent[node] = p->parent[p->parent[node]]; node = p->parent[node]; }
    return node;
}
static void join(kms_desktop_routes_t *p, unsigned a, unsigned b) { p->parent[root(p, a)] = (uint8_t)root(p, b); }
static int32_t half(int32_t n) { return n / 2 - (n < 0 && n % 2 != 0); }
static int32_t center(const kms_route_node_t *n, bool vertical)
{ return vertical ? half(n->bounds.y + bottom(&n->bounds)) : half(n->bounds.x + right(&n->bounds)); }
static unsigned facing(const kms_route_node_t *a, const kms_route_node_t *b)
{
    int32_t dx = center(b, false) - center(a, false), dy = center(b, true) - center(a, true);
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    return ax >= ay ? (dx >= 0 ? KMS_D_RIGHT : KMS_D_LEFT) : (dy >= 0 ? KMS_D_DOWN : KMS_D_UP);
}
static void connect(kms_desktop_routes_t *p, unsigned a, unsigned b, unsigned direction)
{ p->links[a][b] |= (uint8_t)(1u << direction); p->links[b][a] |= (uint8_t)(1u << (direction ^ 1u)); }
static int contact(const kms_desktop_rect_t *a, const kms_desktop_rect_t *b)
{
    if (a->width <= 0 || a->height <= 0 || b->width <= 0 || b->height <= 0) return -1;
    if (minimum(bottom(a), bottom(b)) > maximum(a->y, b->y)) {
        if (right(a) == b->x) return KMS_D_RIGHT;
        if (right(b) == a->x) return KMS_D_LEFT;
    }
    if (minimum(right(a), right(b)) > maximum(a->x, b->x)) {
        if (bottom(a) == b->y) return KMS_D_DOWN;
        if (bottom(b) == a->y) return KMS_D_UP;
    }
    return -1;
}
static unsigned node_contacts(const kms_desktop_routes_t *p, unsigned a, unsigned b)
{
    const kms_route_node_t *x = &p->nodes[a], *y = &p->nodes[b]; unsigned mask = 0;
    for (unsigned i = 0; i < p->counts[x->slot]; ++i) {
        unsigned r = p->order[x->slot][i]; if (!(x->monitors & (1u << r))) continue;
        for (unsigned j = 0; j < p->counts[y->slot]; ++j) {
            unsigned s = p->order[y->slot][j]; if (!(y->monitors & (1u << s))) continue;
            int direction = contact(&p->world[x->slot][r], &p->world[y->slot][s]); if (direction >= 0) mask |= 1u << direction;
        }
    }
    return mask;
}
static uint64_t node_distance(const kms_desktop_routes_t *p, unsigned a, unsigned b)
{
    const kms_route_node_t *x = &p->nodes[a], *y = &p->nodes[b]; uint64_t best = UINT64_MAX;
    for (unsigned i = 0; i < p->counts[x->slot]; ++i) if (x->monitors & (1u << i))
        for (unsigned j = 0; j < p->counts[y->slot]; ++j) if (y->monitors & (1u << j)) {
            const kms_desktop_rect_t *r = &p->world[x->slot][i], *s = &p->world[y->slot][j];
            int64_t dx = maximum(0, maximum(r->x - right(s), s->x - right(r)));
            int64_t dy = maximum(0, maximum(r->y - bottom(s), s->y - bottom(r)));
            uint64_t distance = (uint64_t)(dx * dx + dy * dy); if (distance < best) best = distance;
        }
    return best;
}
static int virtual_direction(kms_desktop_routes_t *p, unsigned a, unsigned b);

static void bypass(kms_desktop_routes_t *p)
{
    for (unsigned i = 0; i < p->count; ++i) p->parent[i] = (uint8_t)i;
    for (unsigned i = 0; i < p->count; ++i) if (!active(p, i))
        for (unsigned j = i + 1; j < p->count; ++j) if (!active(p, j) && p->links[i][j]) join(p, i, j);
    uint8_t boundary[KMS_ROUTE_NODES];
    for (unsigned group = 0; group < p->count; ++group) if (!active(p, group) && root(p, group) == group) {
        memset(boundary, 0, sizeof(boundary));
        for (unsigned i = 0; i < p->count; ++i) if (!active(p, i) && root(p, i) == group)
            for (unsigned j = 0; j < p->count; ++j) if (active(p, j) && p->links[i][j]) boundary[j] = 1;
        for (unsigned i = 0; i < p->count; ++i) if (boundary[i])
            for (unsigned j = i + 1; j < p->count; ++j) if (boundary[j] && p->nodes[i].slot != p->nodes[j].slot && !p->links[i][j]) {
                int d = virtual_direction(p, i, j);
                if (d >= 0) connect(p, i, j, (unsigned)d);
            }
    }
}
static bool better(const kms_desktop_routes_t *p, unsigned group, uint64_t distance, unsigned a, unsigned b)
{
    return distance < p->best_distance[group] || (distance == p->best_distance[group] &&
        (a < p->best_a[group] || (a == p->best_a[group] && b < p->best_b[group])));
}
static void repair(kms_desktop_routes_t *p)
{
    for (unsigned i = 0; i < p->count; ++i) p->parent[i] = (uint8_t)i;
    for (unsigned i = 0; i < p->count; ++i) for (unsigned j = i + 1; j < p->count; ++j)
        if (active(p, i) && active(p, j) && p->links[i][j]) join(p, i, j);
    // Boruvka selects the same uniquely ordered spanning edges as Kruskal,
    // without storing/sorting O(monitors squared) weighted candidate records.
    for (;;) {
        for (unsigned i = 0; i < p->count; ++i) { p->best_distance[i] = UINT64_MAX; p->best_a[i] = p->best_b[i] = UINT8_MAX; }
        for (unsigned i = 0; i < p->count; ++i) if (active(p, i))
            for (unsigned j = i + 1; j < p->count; ++j) if (active(p, j) && p->nodes[i].slot != p->nodes[j].slot) {
                unsigned a = root(p, i), b = root(p, j); if (a == b) continue;
                if (virtual_direction(p, i, j) < 0) continue;
                uint64_t distance = node_distance(p, i, j);
                if (better(p, a, distance, i, j)) { p->best_distance[a] = distance; p->best_a[a] = (uint8_t)i; p->best_b[a] = (uint8_t)j; }
                if (better(p, b, distance, i, j)) { p->best_distance[b] = distance; p->best_a[b] = (uint8_t)i; p->best_b[b] = (uint8_t)j; }
            }
        bool changed = false;
        for (unsigned i = 0; i < p->count; ++i) if (p->best_a[i] != UINT8_MAX) {
            unsigned a = p->best_a[i], b = p->best_b[i]; if (root(p, a) == root(p, b)) continue;
            int d = virtual_direction(p, a, b);
            if (d >= 0) { connect(p, a, b, (unsigned)d); join(p, a, b); changed = true; }
        }
        if (!changed) return;
    }
}
static bool edge_before(const kms_route_edge_t *a, const kms_route_edge_t *b, bool fixed_first)
{
    int32_t x = fixed_first ? a->fixed : a->start, y = fixed_first ? b->fixed : b->start;
    if (x != y) return x < y;
    x = fixed_first ? a->start : a->fixed; y = fixed_first ? b->start : b->fixed;
    return x != y ? x < y : a->end < b->end;
}
static void sort_edges(kms_route_edge_t *list, unsigned count, bool fixed_first)
{
    for (unsigned i = 1; i < count; ++i) {
        kms_route_edge_t edge = list[i]; unsigned j = i;
        while (j && edge_before(&edge, &list[j - 1], fixed_first)) { list[j] = list[j - 1]; --j; }
        list[j] = edge;
    }
}
static unsigned edges(kms_desktop_routes_t *p, unsigned index, unsigned direction)
{
    const kms_route_node_t *node = &p->nodes[index]; unsigned slot = node->slot, count = 0;
    int32_t corner = p->config.corner;
    for (unsigned i = 0; i < p->counts[slot]; ++i) if (node->monitors & (1u << i)) {
        const kms_desktop_rect_t *r = &p->local[slot][i];
        int32_t fixed = direction == KMS_D_LEFT ? r->x : direction == KMS_D_RIGHT ? right(r) - 1 : direction == KMS_D_UP ? r->y : bottom(r) - 1;
        int32_t start = (direction < KMS_D_UP ? r->y : r->x) + corner;
        int32_t end = (direction < KMS_D_UP ? bottom(r) : right(r)) - corner;
        kms_route_edge_t intervals[KMS_D_MONITORS + 1], next[KMS_D_MONITORS + 1];
        unsigned length = start < end ? 1 : 0; intervals[0] = (kms_route_edge_t){fixed, start, end};
        int32_t outside = fixed + ((direction & 1u) ? 1 : -1);
        for (unsigned j = 0; j < p->counts[slot] && length; ++j) {
            const kms_desktop_rect_t *q = &p->local[slot][j];
            bool covered = direction < KMS_D_UP ? q->x <= outside && outside < right(q) : q->y <= outside && outside < bottom(q);
            if (!covered) continue;
            int32_t lo = direction < KMS_D_UP ? q->y : q->x, hi = direction < KMS_D_UP ? bottom(q) : right(q);
            unsigned n = 0;
            for (unsigned k = 0; k < length; ++k) {
                kms_route_edge_t edge = intervals[k];
                if (edge.start < minimum(edge.end, lo)) next[n++] = (kms_route_edge_t){fixed, edge.start, minimum(edge.end, lo)};
                if (maximum(edge.start, hi) < edge.end) next[n++] = (kms_route_edge_t){fixed, maximum(edge.start, hi), edge.end};
            }
            length = n; memcpy(intervals, next, n * sizeof(next[0]));
        }
        if (count + length > KMS_ROUTE_EDGES) return 0;
        memcpy(p->edges + count, intervals, length * sizeof(intervals[0])); count += length;
    }
    sort_edges(p->edges, count, true); unsigned merged = 0;
    for (unsigned i = 0; i < count; ++i) {
        kms_route_edge_t e = p->edges[i];
        if (merged && p->edges[merged - 1].fixed == e.fixed && e.start <= p->edges[merged - 1].end)
            p->edges[merged - 1].end = maximum(p->edges[merged - 1].end, e.end);
        else p->edges[merged++] = e;
    }
    sort_edges(p->edges, merged, false); return merged;
}
// Project only actual monitor boundaries into local pixel coordinates. Between
// consecutive cuts, physical contacts and intervening-PC visibility are constant.
static int32_t local_cut(const kms_desktop_routes_t *p, unsigned slot, unsigned d, int32_t value)
{
    const kms_desktop_rect_t *box = &p->desktop[slot];
    const kms_desktop_station_t *station = &p->config.stations[slot];
    int32_t from = d < KMS_D_UP ? box->height : box->width;
    int32_t to = d < KMS_D_UP ? station->height : station->width; if (!to) to = from;
    int32_t origin = d < KMS_D_UP ? station->y : station->x;
    int64_t numerator = (int64_t)(value - origin) * from;
    int32_t offset = (int32_t)(numerator / to + (numerator > 0 && numerator % to != 0));
    return (d < KMS_D_UP ? box->y : box->x) + offset;
}
static unsigned cuts(const kms_desktop_routes_t *p, unsigned from, unsigned d,
                     const kms_route_edge_t *edge, int32_t *values)
{
    unsigned count = 2; values[0] = edge->start; values[1] = edge->end;
    for (unsigned slot = 0; slot < KMS_SLOTS; ++slot) if (p->barriers & (1u << slot))
        for (unsigned i = 0; i < p->counts[slot]; ++i) {
            const kms_desktop_rect_t *r = &p->world[slot][i];
            for (unsigned end = 0; end < 2; ++end) {
                int32_t world = d < KMS_D_UP ? (end ? bottom(r) : r->y) : (end ? right(r) : r->x);
                int32_t value = local_cut(p, p->nodes[from].slot, d, world);
                if (value <= edge->start || value >= edge->end) continue;
                unsigned at = 0; while (at < count && values[at] < value) ++at;
                if (at < count && values[at] == value) continue;
                for (unsigned j = count; j > at; --j) values[j] = values[j - 1];
                values[at] = value; ++count;
            }
        }
    return count;
}
static void world_point(const kms_desktop_routes_t *p, unsigned from, unsigned d,
                        int32_t x, int32_t y, int32_t *wx, int32_t *wy)
{
    unsigned slot = p->nodes[from].slot; const kms_desktop_rect_t *b = &p->desktop[slot];
    const kms_desktop_station_t *s = &p->config.stations[slot];
    *wx = s->x + scale(x - b->x + (d == KMS_D_RIGHT), b->width, s->width ? s->width : b->width);
    *wy = s->y + scale(y - b->y + (d == KMS_D_DOWN), b->height, s->height ? s->height : b->height);
}
static bool physical_at(const kms_desktop_routes_t *p, unsigned to, unsigned d, int32_t wx, int32_t wy)
{
    const kms_route_node_t *node = &p->nodes[to];
    int32_t outside_x = wx - (d == KMS_D_RIGHT), outside_y = wy - (d == KMS_D_DOWN);
    for (unsigned i = 0; i < p->counts[node->slot]; ++i)
        if (contains(&p->world[node->slot][i], outside_x, outside_y)) return false;
    for (unsigned i = 0; i < p->counts[node->slot]; ++i) if (node->monitors & (1u << i)) {
        const kms_desktop_rect_t *r = &p->world[node->slot][i];
        if (r->width <= 0 || r->height <= 0) continue;
        if (d == KMS_D_RIGHT && wx == r->x && wy >= r->y && wy < bottom(r)) return true;
        if (d == KMS_D_LEFT && wx == right(r) && wy >= r->y && wy < bottom(r)) return true;
        if (d == KMS_D_DOWN && wy == r->y && wx >= r->x && wx < right(r)) return true;
        if (d == KMS_D_UP && wy == bottom(r) && wx >= r->x && wx < right(r)) return true;
    }
    return false;
}
static int32_t edge_plane(const kms_desktop_rect_t *r, unsigned d)
{
    return d == KMS_D_LEFT ? r->x : d == KMS_D_RIGHT ? right(r) : d == KMS_D_UP ? r->y : bottom(r);
}
static int32_t edge_start(const kms_desktop_rect_t *r, unsigned d)
{ return d < KMS_D_UP ? r->y : r->x; }
static int32_t edge_end(const kms_desktop_rect_t *r, unsigned d)
{ return d < KMS_D_UP ? bottom(r) : right(r); }
static int32_t edge_gap(int32_t from, int32_t to, unsigned d)
{ return (d & 1u) ? to - from : from - to; }

static int32_t covered_until(const kms_desktop_rect_t *screens, unsigned count, unsigned d,
                             int32_t plane, int32_t along)
{
    int32_t outside = plane - !(d & 1u), end = along;
    for (unsigned i = 0; i < count; ++i) {
        const kms_desktop_rect_t *r = &screens[i];
        int32_t x = d < KMS_D_UP ? outside : along, y = d < KMS_D_UP ? along : outside;
        if (contains(r, x, y)) end = maximum(end, edge_end(r, d));
    }
    return end;
}
static bool within_tolerance(int32_t gap, uint32_t gaps, uint32_t overlaps)
{ return gap < 0 ? (uint32_t)-gap <= overlaps : (uint32_t)gap <= gaps; }

static bool facing_edges(const kms_desktop_rect_t *a, const kms_desktop_rect_t *b, unsigned d, int32_t gap)
{
    // Each facing edge must remain outside the far edge of the other screen.
    int32_t depth = d < KMS_D_UP ? minimum(a->width, b->width) : minimum(a->height, b->height);
    return depth > 0 && gap > -depth &&
        maximum(edge_start(a, d), edge_start(b, d)) < minimum(edge_end(a, d), edge_end(b, d));
}
static int32_t corridor_covered_until(const kms_desktop_rect_t screens[][KMS_D_MONITORS],
                                     const uint8_t counts[KMS_SLOTS], unsigned stations,
                                     unsigned a, unsigned i, unsigned b, unsigned j, unsigned d,
                                     int32_t plane, int32_t face, int32_t along)
{
    int32_t low = minimum(plane, face), high = maximum(plane, face), covered = along;
    for (unsigned slot = 0; slot < KMS_SLOTS; ++slot) if (stations & (1u << slot)) {
        for (unsigned index = 0; index < counts[slot]; ++index) {
            if ((slot == a && index == i) || (slot == b && index == j)) continue;
            const kms_desktop_rect_t *r = &screens[slot][index];
            if (r->width <= 0 || r->height <= 0 || along < edge_start(r, d) || along >= edge_end(r, d))
                continue;
            int32_t start = edge_plane(r, d & ~1u), end = edge_plane(r, d | 1u);
            if (start <= high && end >= low) covered = maximum(covered, edge_end(r, d));
        }
    }
    return covered;
}
static bool reference_contact(const kms_desktop_routes_t *p, unsigned a, unsigned i,
                              unsigned b, unsigned j, unsigned d)
{
    const kms_desktop_rect_t *from = &p->reference[a][i], *to = &p->reference[b][j];
    unsigned reverse = d ^ 1u;
    int32_t start = maximum(edge_start(from, d), edge_start(to, d));
    int32_t end = minimum(edge_end(from, d), edge_end(to, d));
    int32_t first = edge_plane(from, d), second = edge_plane(to, reverse);
    int32_t gap = edge_gap(first, second, d);
    if (!within_tolerance(gap, p->reference_gap, p->reference_overlap) ||
        !facing_edges(from, to, d, gap)) return false;
    while (start < end) {
        int32_t covered = maximum(covered_until(p->reference[a], p->reference_counts[a], d, first, start),
            covered_until(p->reference[b], p->reference_counts[b], reverse, second, start));
        if (covered == start)
            covered = corridor_covered_until(p->reference, p->reference_counts, UINT8_MAX,
                                             a, i, b, j, d, first, second, start);
        if (covered == start) return true;
        start = covered;
    }
    return false;
}
static unsigned bridge_directions(const kms_desktop_routes_t *p, unsigned from, unsigned to)
{
    const kms_route_node_t *a = &p->nodes[from], *b = &p->nodes[to];
    if (!p->config.gap_tolerance && !p->config.overlap_tolerance &&
        (!p->reference_counts[a->slot] || !p->reference_counts[b->slot])) return 0;
    unsigned mask = 0;
    for (unsigned i = 0; i < p->counts[a->slot]; ++i) {
        unsigned r = p->roles[a->slot][i];
        if (!(a->monitors & (1u << i))) continue;
        const kms_desktop_rect_t *first = &p->world[a->slot][i];
        for (unsigned j = 0; j < p->counts[b->slot]; ++j) {
            unsigned s = p->roles[b->slot][j];
            if (!(b->monitors & (1u << j))) continue;
            const kms_desktop_rect_t *second = &p->world[b->slot][j];
            for (unsigned d = 0; d < 4; ++d) {
                if (mask & (1u << d)) continue;
                int32_t gap = edge_gap(edge_plane(first, d), edge_plane(second, d ^ 1u), d);
                if (gap && facing_edges(first, second, d, gap) &&
                    (within_tolerance(gap, p->config.gap_tolerance, p->config.overlap_tolerance) ||
                     (r != UINT8_MAX && s != UINT8_MAX && reference_contact(p, a->slot, r, b->slot, s, d))))
                    mask |= 1u << d;
            }
        }
    }
    return mask;
}
static bool is_bridged(const kms_desktop_routes_t *p, unsigned from, unsigned to)
{ return (p->bridged[from][to / 8] & (1u << (to % 8))) != 0; }

static int32_t ahead(const kms_desktop_routes_t *p, unsigned slot, uint16_t mask, unsigned d, int32_t wx, int32_t wy)
{
    int32_t nearest = INT32_MAX, plane = d < KMS_D_UP ? wx : wy, along = d < KMS_D_UP ? wy : wx;
    for (unsigned i = 0; i < p->counts[slot]; ++i) if (mask & (1u << i)) {
        const kms_desktop_rect_t *r = &p->world[slot][i]; if (r->width <= 0 || r->height <= 0) continue;
        int32_t lo = d < KMS_D_UP ? r->x : r->y, hi = d < KMS_D_UP ? right(r) : bottom(r);
        int32_t start = d < KMS_D_UP ? r->y : r->x, end = d < KMS_D_UP ? bottom(r) : right(r);
        if (along < start || along >= end) continue;
        if ((d & 1u) && hi > plane) nearest = minimum(nearest, maximum(0, lo - plane));
        if (!(d & 1u) && lo < plane) nearest = minimum(nearest, maximum(0, plane - hi));
    }
    return nearest;
}
static bool visible(const kms_desktop_routes_t *p, unsigned from, unsigned to, unsigned d, int32_t wx, int32_t wy)
{
    const kms_route_node_t *target = &p->nodes[to];
    int32_t distance = ahead(p, target->slot, target->monitors, d, wx, wy);
    for (unsigned slot = 0; slot < KMS_SLOTS; ++slot)
        if ((p->barriers & (1u << slot)) && slot != p->nodes[from].slot && slot != target->slot) {
            int32_t nearer = ahead(p, slot, UINT16_MAX, d, wx, wy);
            if (nearer != INT32_MAX && (distance == INT32_MAX || nearer < distance)) return false;
        }
    return true;
}
static int bridge_at(const kms_desktop_routes_t *p, unsigned from, unsigned to,
                     unsigned d, int32_t wx, int32_t wy, unsigned *kind)
{
    const kms_route_node_t *source = &p->nodes[from], *target = &p->nodes[to];
    unsigned a = source->slot, b = target->slot, reverse = d ^ 1u;
    int32_t plane = d < KMS_D_UP ? wx : wy, along = d < KMS_D_UP ? wy : wx;
    int match = -1;
    for (unsigned i = 0; i < p->counts[a]; ++i) {
        const kms_desktop_rect_t *r = &p->world[a][i];
        unsigned role = p->roles[a][i];
        if (!(source->monitors & (1u << i)) || edge_plane(r, d) != plane ||
            along < edge_start(r, d) || along >= edge_end(r, d)) continue;
        for (unsigned j = 0; j < p->counts[b]; ++j) {
            const kms_desktop_rect_t *s = &p->world[b][j];
            unsigned other = p->roles[b][j];
            int32_t face = edge_plane(s, reverse), gap = edge_gap(plane, face, d);
            if (!(target->monitors & (1u << j)) || !gap || !facing_edges(r, s, d, gap) ||
                along < edge_start(s, d) || along >= edge_end(s, d) ||
                covered_until(p->world[b], p->counts[b], reverse, face, along) != along) continue;
            bool near = within_tolerance(gap, p->config.gap_tolerance, p->config.overlap_tolerance);
            if (!near && (role == UINT8_MAX || other == UINT8_MAX ||
                         !reference_contact(p, a, role, b, other, d))) continue;
            if (corridor_covered_until(p->world, p->counts, p->barriers, a, i, b, j, d, plane, face, along) != along)
                continue;
            if (match >= 0) return -1;
            match = (int)j;
            if (kind) *kind = near ? KMS_D_ROUTE_TOLERANCE : KMS_D_ROUTE_REMEMBERED;
        }
    }
    return match;
}
enum contact_kind { PHYSICAL, BRIDGED, TEMPORARY };
static bool can_cross(kms_desktop_routes_t *p, unsigned from, unsigned to, unsigned d, enum contact_kind kind)
{
    unsigned count = edges(p, from, d); int32_t values[2 * KMS_ROUTE_NODES + 2];
    for (unsigned i = 0; i < count; ++i) {
        kms_route_edge_t edge = p->edges[i]; unsigned n = cuts(p, from, d, &edge, values);
        for (unsigned j = 0; j + 1 < n; ++j) {
            int32_t x = d < KMS_D_UP ? edge.fixed : values[j], y = d < KMS_D_UP ? values[j] : edge.fixed, wx, wy;
            world_point(p, from, d, x, y, &wx, &wy);
            bool reserved = false;
            if (kind != PHYSICAL) for (unsigned k = 0; k < p->count; ++k) {
                if (!active(p, k) || !(p->links[from][k] & (1u << d))) continue;
                if (((p->links[from][k] & (1u << (d + 4))) && physical_at(p, k, d, wx, wy)) ||
                    (kind == TEMPORARY && is_bridged(p, from, k) && bridge_at(p, from, k, d, wx, wy, NULL) >= 0)) {
                    reserved = true; break;
                }
            }
            if (reserved) continue;
            if (kind == PHYSICAL && physical_at(p, to, d, wx, wy)) return true;
            if (kind == BRIDGED && bridge_at(p, from, to, d, wx, wy, NULL) >= 0) return true;
            if (kind == TEMPORARY && visible(p, from, to, d, wx, wy)) return true;
        }
    }
    return false;
}
static int virtual_direction(kms_desktop_routes_t *p, unsigned a, unsigned b)
{
    unsigned first = facing(&p->nodes[a], &p->nodes[b]);
    const unsigned order[] = {first, first ^ 2u, first ^ 3u, first ^ 1u};
    for (unsigned i = 0; i < 4; ++i) {
        unsigned d = order[i];
        if (can_cross(p, a, b, d, TEMPORARY) && can_cross(p, b, a, d ^ 1u, TEMPORARY)) return (int)d;
    }
    return -1;
}
static bool target_before(const kms_desktop_routes_t *p, unsigned a, unsigned b, unsigned direction)
{
    int32_t x = center(&p->nodes[a], direction < KMS_D_UP), y = center(&p->nodes[b], direction < KMS_D_UP);
    return x != y ? x < y : a < b;
}
static unsigned build_portal(kms_desktop_routes_t *p, unsigned from, unsigned to, unsigned d, unsigned filter)
{
    unsigned count = edges(p, from, d), used = 0; int32_t values[2 * KMS_ROUTE_NODES + 2];
    for (unsigned i = 0; i < count; ++i) {
        kms_route_edge_t edge = p->edges[i]; unsigned n = cuts(p, from, d, &edge, values);
        for (unsigned j = 0; j + 1 < n; ++j) {
            int32_t x = d < KMS_D_UP ? edge.fixed : values[j], y = d < KMS_D_UP ? values[j] : edge.fixed, wx, wy;
            world_point(p, from, d, x, y, &wx, &wy);
            uint8_t candidates[KMS_ROUTE_NODES]; unsigned number = 0, kind = KMS_D_ROUTE_EXACT;
            // Exact contacts have priority on their full exposed spans.
            for (unsigned k = 0; k < p->count; ++k) if (active(p, k) &&
                (p->links[from][k] & (1u << (d + 4))) && physical_at(p, k, d, wx, wy)) candidates[number++] = (uint8_t)k;
            if (!number) {
                for (unsigned k = 0; k < p->count; ++k) if (active(p, k) &&
                    (p->links[from][k] & (1u << d)) && is_bridged(p, from, k) &&
                    bridge_at(p, from, k, d, wx, wy, &kind) >= 0) candidates[number++] = (uint8_t)k;
                if (number > 1) number = 0; // Competing bridge destinations are ambiguous.
            }
            if (!number) {
                kind = KMS_D_ROUTE_FALLBACK;
                for (unsigned k = 0; k < p->count; ++k) if (active(p, k) &&
                    (p->links[from][k] & (1u << d)) && !(p->links[from][k] & (1u << (d + 4))) &&
                    !is_bridged(p, from, k) && visible(p, from, k, d, wx, wy)) candidates[number++] = (uint8_t)k;
            }
            if (filter && filter != kind) continue;
            bool found = false; unsigned rank = 0;
            for (unsigned k = 0; k < number; ++k) {
                if (candidates[k] == to) found = true;
                if (target_before(p, candidates[k], to, d)) ++rank;
            }
            if (!found) continue;
            uint32_t length = (uint32_t)(values[j + 1] - values[j]);
            int32_t lo = values[j] + (int32_t)(rank * length / number);
            int32_t hi = values[j] + (int32_t)((rank + 1) * length / number);
            if (lo < hi) {
                if (used && p->portal[used - 1].fixed == edge.fixed && p->portal[used - 1].end == lo) {
                    p->portal[used - 1].end = hi; continue;
                }
                if (used == KMS_ROUTE_EDGES) return 0;
                p->portal[used++] = (kms_route_edge_t){edge.fixed, lo, hi};
            }
        }
    }
    sort_edges(p->portal, used, true); unsigned merged = 0;
    for (unsigned i = 0; i < used; ++i) {
        kms_route_edge_t edge = p->portal[i];
        if (merged && p->portal[merged - 1].fixed == edge.fixed && edge.start <= p->portal[merged - 1].end)
            p->portal[merged - 1].end = maximum(p->portal[merged - 1].end, edge.end);
        else p->portal[merged++] = edge;
    }
    sort_edges(p->portal, merged, false); return merged;
}
static unsigned portal_edges(kms_desktop_routes_t *p, unsigned from, unsigned to, unsigned d)
{ return build_portal(p, from, to, d, 0); }

bool kms_desktop_routes_preview(kms_desktop_routes_t *p, unsigned slot, uint32_t cursor, bool all,
                                kms_desktop_route_preview_t *preview)
{
    enum { BAND_BITS = 9, BAND_MASK = (1u << BAND_BITS) - 1, KINDS = 4 };
    _Static_assert((unsigned)KMS_ROUTE_EDGES <= BAND_MASK, "Route preview cursor needs more band bits");
    unsigned index = cursor >> BAND_BITS, band = cursor & BAND_MASK;
    for (; index < p->count * p->count * 4u * KINDS; ++index, band = 0) {
        unsigned link = index / KINDS, kind = 1 + index % KINDS;
        unsigned from = link / (4u * p->count), to = (link / 4u) % p->count, d = link % 4u;
        if ((unsigned)p->nodes[from].slot + 1 != slot || !active(p, from) || !active(p, to) ||
            !(p->links[from][to] & (1u << d))) continue;
        bool bridged = is_bridged(p, from, to), exact = (p->links[from][to] & (1u << (d + 4))) != 0;
        if (kind == KMS_D_ROUTE_EXACT && (!all || !exact)) continue;
        if (kind == KMS_D_ROUTE_FALLBACK && (!all || bridged || exact)) continue;
        if ((kind == KMS_D_ROUTE_TOLERANCE || kind == KMS_D_ROUTE_REMEMBERED) && !bridged) continue;
        unsigned count = build_portal(p, from, to, d, kind);
        if (band >= count) continue;
        const kms_route_edge_t *edge = &p->portal[band];
        int32_t *line = preview->line;
        if (d < KMS_D_UP) {
            world_point(p, from, d, edge->fixed, edge->start, &line[0], &line[1]);
            world_point(p, from, d, edge->fixed, edge->end, &line[2], &line[3]);
        } else {
            world_point(p, from, d, edge->start, edge->fixed, &line[0], &line[1]);
            world_point(p, from, d, edge->end, edge->fixed, &line[2], &line[3]);
        }
        preview->next = (index << BAND_BITS) + band + 1;
        preview->kind = (uint8_t)kind;
        preview->target = p->nodes[to].slot + 1;
        preview->direction = (uint8_t)d;
        return true;
    }
    return false;
}
static uint32_t portal_length(const kms_desktop_routes_t *p, unsigned count)
{
    uint32_t length = 0;
    for (unsigned i = 0; i < count; ++i) length += (uint32_t)(p->portal[i].end - p->portal[i].start);
    return length;
}
bool kms_desktop_routes_current(const kms_desktop_routes_t *p, const kms_desktop_master_t *m, uint8_t mask)
{
    if (!p || !p->built || p->revision != m->topology_revision || p->active != mask || p->barriers != m->availability || memcmp(&p->config, &m->config, sizeof(p->config))) return false;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) {
        unsigned count = m->config.stations[i].configured ? m->peers[i].count : 0;
        if (p->counts[i] != count || (count && memcmp(p->local[i], m->peers[i].screens, count * sizeof(p->local[i][0])))) return false;
    }
    return true;
}
bool kms_desktop_routes_prepare(kms_desktop_routes_t *p, const kms_desktop_master_t *m, uint8_t mask)
{
    if (!p) return false;
    if (kms_desktop_routes_current(p, m, mask)) return p->valid;
    uint32_t builds = p->builds + 1; memset(p, 0, sizeof(*p));
    p->builds = builds; p->built = true; p->revision = m->topology_revision; p->active = mask; p->barriers = m->availability;
    memcpy(&p->config, &m->config, sizeof(p->config));
    p->reference_gap = m->reference_gap; p->reference_overlap = m->reference_overlap;
    for (unsigned i = 0; i < KMS_SLOTS; ++i) station_nodes(p, m, i);
    for (unsigned i = 0; i < p->count; ++i) for (unsigned j = i + 1; j < p->count; ++j)
        if (p->nodes[i].slot != p->nodes[j].slot) {
            unsigned directions = node_contacts(p, i, j);
            for (unsigned d = 0; d < 4; ++d) if ((directions & (1u << d)) &&
                (!(active(p, i) && active(p, j)) || (can_cross(p, i, j, d, PHYSICAL) && can_cross(p, j, i, d ^ 1u, PHYSICAL)))) {
                connect(p, i, j, d);
                p->links[i][j] |= (uint8_t)(1u << (d + 4));
                p->links[j][i] |= (uint8_t)(1u << ((d ^ 1u) + 4));
            }
        }
    for (unsigned i = 0; i < p->count; ++i) if (active(p, i))
        for (unsigned j = i + 1; j < p->count; ++j)
            if (active(p, j) && p->nodes[i].slot != p->nodes[j].slot) {
                unsigned directions = bridge_directions(p, i, j);
                for (unsigned d = 0; d < 4; ++d) if ((directions & (1u << d)) &&
                    can_cross(p, i, j, d, BRIDGED) && can_cross(p, j, i, d ^ 1u, BRIDGED)) {
                    connect(p, i, j, d);
                    p->bridged[i][j / 8] |= (uint8_t)(1u << (j % 8));
                    p->bridged[j][i / 8] |= (uint8_t)(1u << (i % 8));
                }
            }
    // A competing remembered join can exhaust a very narrow span. Drop that
    // join before connectivity repair instead of invalidating the whole desk.
    for (unsigned i = 0; i < p->count; ++i) for (unsigned j = i + 1; j < p->count; ++j)
        if (is_bridged(p, i, j)) {
            for (unsigned d = 0; d < 4; ++d)
                if ((p->links[i][j] & (1u << d)) && !(p->links[i][j] & (1u << (d + 4))) &&
                    (!portal_edges(p, i, j, d) || !portal_edges(p, j, i, d ^ 1u))) {
                    p->links[i][j] &= (uint8_t)~(1u << d);
                    p->links[j][i] &= (uint8_t)~(1u << (d ^ 1u));
                }
            if (!p->links[i][j]) {
                p->bridged[i][j / 8] &= (uint8_t)~(1u << (j % 8));
                p->bridged[j][i / 8] &= (uint8_t)~(1u << (i % 8));
            }
        }
    bypass(p); repair(p);
    unsigned slots = 0, first = p->count;
    for (unsigned slot = 0; slot < KMS_SLOTS; ++slot) if (mask & (1u << slot)) ++slots;
    for (unsigned i = 0; i < p->count; ++i) if (active(p, i)) {
        if (first == p->count) first = i;
        if (slots > 1 && root(p, i) != root(p, first)) return false;
        for (unsigned j = 0; j < p->count; ++j) if (active(p, j))
            for (unsigned d = 0; d < 4; ++d) if ((p->links[i][j] & (1u << d)) && !portal_edges(p, i, j, d)) return false;
    }
    p->valid = true; return true;
}
static bool direct_landing(const kms_desktop_routes_t *p, unsigned from, unsigned to, unsigned d,
                           int32_t x, int32_t y, int32_t *lx, int32_t *ly)
{
    unsigned source = p->nodes[from].slot, target = p->nodes[to].slot;
    kms_desktop_rect_t sb = p->desktop[source];
    kms_desktop_rect_t tb = p->desktop[target];
    const kms_desktop_station_t *s = &p->config.stations[source], *t = &p->config.stations[target];
    int32_t sw = s->width ? s->width : sb.width, sh = s->height ? s->height : sb.height;
    int32_t tw = t->width ? t->width : tb.width, th = t->height ? t->height : tb.height;
    int32_t wx = s->x + scale(x - sb.x + (d == KMS_D_RIGHT), sb.width, sw);
    int32_t wy = s->y + scale(y - sb.y + (d == KMS_D_DOWN), sb.height, sh);
    int bridged = is_bridged(p, from, to) && !physical_at(p, to, d, wx, wy) ?
        bridge_at(p, from, to, d, wx, wy, NULL) : -1;
    for (unsigned i = 0; i < p->counts[target]; ++i) if (p->nodes[to].monitors & (1u << i)) {
        const kms_desktop_rect_t *r = &p->world[target][i], *local = &p->local[target][i];
        bool aligned = d == KMS_D_RIGHT ? wx == r->x && wy >= r->y && wy < bottom(r) :
            d == KMS_D_LEFT ? wx == right(r) && wy >= r->y && wy < bottom(r) :
            d == KMS_D_DOWN ? wy == r->y && wx >= r->x && wx < right(r) :
            wy == bottom(r) && wx >= r->x && wx < right(r);
        if (!aligned && (int)i != bridged) continue;
        *lx = d == KMS_D_RIGHT ? local->x + 2 : d == KMS_D_LEFT ? right(local) - 3 : tb.x + scale(wx - t->x, tw, tb.width);
        *ly = d == KMS_D_DOWN ? local->y + 2 : d == KMS_D_UP ? bottom(local) - 3 : tb.y + scale(wy - t->y, th, tb.height);
        *lx = maximum(local->x + 2, minimum(*lx, right(local) - 3));
        *ly = maximum(local->y + 2, minimum(*ly, bottom(local) - 3));
        return true;
    }
    return false;
}
bool kms_desktop_routes_cross(kms_desktop_routes_t *p, unsigned source, unsigned direction, int32_t x, int32_t y,
                             unsigned *target, int32_t *landing_x, int32_t *landing_y)
{
    if (!p || !p->valid || !source || source > KMS_SLOTS || direction > KMS_D_DOWN) return false;
    unsigned from = p->count;
    for (unsigned i = 0; i < p->count && from == p->count; ++i) if ((unsigned)p->nodes[i].slot + 1 == source && active(p, i))
        for (unsigned j = 0; j < p->counts[source - 1]; ++j)
            if ((p->nodes[i].monitors & (1u << j)) && contains(&p->local[source - 1][j], x, y)) { from = i; break; }
    if (from == p->count) return false;
    for (unsigned to = 0; to < p->count; ++to) if (active(p, to) && (p->links[from][to] & (1u << direction))) {
        unsigned count = portal_edges(p, from, to, direction); uint32_t offset = 0, total = portal_length(p, count); bool found = false;
        for (unsigned i = 0; i < count; ++i) {
            const kms_route_edge_t *edge = &p->portal[i];
            int32_t fixed = direction < KMS_D_UP ? x : y, along = direction < KMS_D_UP ? y : x;
            if (fixed == edge->fixed && along >= edge->start && along < edge->end) {
                offset += (uint32_t)(along - edge->start); found = true; break;
            }
            offset += (uint32_t)(edge->end - edge->start);
        }
        if (!found) continue;
        if (direct_landing(p, from, to, direction, x, y, landing_x, landing_y)) {
            *target = p->nodes[to].slot + 1; return true;
        }
        unsigned reverse = direction ^ 1u, length = portal_edges(p, to, from, reverse);
        uint32_t capacity = portal_length(p, length);
        if (!total || !capacity) return false;
        uint32_t mapped = (uint32_t)((uint64_t)offset * capacity / total);
        for (unsigned j = 0; j < length; ++j) {
            const kms_route_edge_t *edge = &p->portal[j]; uint32_t size = (uint32_t)(edge->end - edge->start);
            if (mapped >= size) { mapped -= size; continue; }
            int32_t along = edge->start + (int32_t)mapped;
            *landing_x = reverse < KMS_D_UP ? edge->fixed : along;
            *landing_y = reverse < KMS_D_UP ? along : edge->fixed;
            *landing_x += 2 * ((reverse == KMS_D_LEFT) - (reverse == KMS_D_RIGHT));
            *landing_y += 2 * ((reverse == KMS_D_UP) - (reverse == KMS_D_DOWN));
            *target = p->nodes[to].slot + 1; return true;
        }
        return false;
    }
    return false;
}
