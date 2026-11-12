#pragma once
#include "kms_desktop.h"

enum { KMS_ROUTE_NODES = KMS_SLOTS * KMS_D_MONITORS,
       KMS_ROUTE_EDGES = KMS_D_MONITORS * (KMS_D_MONITORS + 1), KMS_ROUTE_BUDGET = 32768 };
typedef struct { int32_t fixed, start, end; } kms_route_edge_t;
typedef struct {
    uint16_t monitors;
    uint8_t slot, ordinal;
    kms_desktop_rect_t bounds;
} kms_route_node_t;

// Caller-owned, task-only storage. The S3 allocates it once, preferably in PSRAM.
// No graph or per-pixel edge tables are allocated on the application stack.
typedef struct kms_desktop_routes {
    bool built, valid;
    // A failed arrival may exclude an endpoint; it must not erase an awake PC as a barrier.
    uint8_t active, barriers, count, counts[KMS_SLOTS], order[KMS_SLOTS][KMS_D_MONITORS];
    uint32_t revision, builds;
    kms_desktop_config_t config;
    kms_desktop_rect_t desktop[KMS_SLOTS];
    kms_desktop_rect_t local[KMS_SLOTS][KMS_D_MONITORS], world[KMS_SLOTS][KMS_D_MONITORS];
    kms_desktop_rect_t reference[KMS_SLOTS][KMS_D_MONITORS];
    uint8_t reference_counts[KMS_SLOTS], roles[KMS_SLOTS][KMS_D_MONITORS];
    uint32_t reference_gap, reference_overlap;
    uint8_t bridged[KMS_ROUTE_NODES][(KMS_ROUTE_NODES + 7) / 8];
    kms_route_node_t nodes[KMS_ROUTE_NODES];
    // Low four bits: facing directions. High four: physical shared contacts.
    uint8_t links[KMS_ROUTE_NODES][KMS_ROUTE_NODES];
    uint8_t parent[KMS_ROUTE_NODES], best_a[KMS_ROUTE_NODES], best_b[KMS_ROUTE_NODES];
    uint64_t best_distance[KMS_ROUTE_NODES];
    kms_route_edge_t edges[KMS_ROUTE_EDGES];
    kms_route_edge_t portal[KMS_ROUTE_EDGES];
} kms_desktop_routes_t;

bool kms_desktop_routes_current(const kms_desktop_routes_t *, const kms_desktop_master_t *, uint8_t active);
bool kms_desktop_routes_prepare(kms_desktop_routes_t *, const kms_desktop_master_t *, uint8_t active);
enum kms_desktop_route_kind { KMS_D_ROUTE_EXACT = 1, KMS_D_ROUTE_TOLERANCE,
    KMS_D_ROUTE_REMEMBERED, KMS_D_ROUTE_FALLBACK };
typedef struct {
    int32_t line[4];
    uint32_t next;
    uint8_t kind, target, direction;
} kms_desktop_route_preview_t;
bool kms_desktop_routes_preview(kms_desktop_routes_t *, unsigned slot, uint32_t cursor,
                                bool all, kms_desktop_route_preview_t *);
bool kms_desktop_routes_cross(kms_desktop_routes_t *, unsigned source, unsigned direction, int32_t x, int32_t y,
                             unsigned *target, int32_t *landing_x, int32_t *landing_y);
