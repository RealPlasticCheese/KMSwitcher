#include "kms_jiggler.h"

const char *kms_jiggler_mode_name(unsigned mode)
{
    static const char *names[] = {"left-right", "four-way", "random"};
    return mode < KMS_JIGGLER_MODE_COUNT ? names[mode] : "invalid";
}
static uint32_t random_next(uint32_t state)
{
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
static unsigned random_phase(const kms_jiggler_t *j, uint32_t random)
{
    unsigned phase = random % 8;
    return phase == j->phase ? (phase + 1) % 8 : phase;
}
bool kms_jiggler_configure(kms_jiggler_t *j, const kms_packet_t *p, uint32_t now)
{
    unsigned mode = (p->flags & KMS_JIGGLER_MODE_MASK) >> KMS_JIGGLER_MODE_SHIFT;
    if (p->type != KMS_JIGGLER || !p->session || !p->sequence ||
        !p->motion[0] || p->motion[0] > KMS_JIGGLER_MAX_SECONDS || p->motion[3] > KMS_JIGGLER_MAX_DISTANCE ||
        mode >= KMS_JIGGLER_MODE_COUNT || (p->flags & ~(KMS_FLAG_JIGGLER_ENABLED | KMS_JIGGLER_MODE_MASK))) return false;
    bool enabled = (p->flags & KMS_FLAG_JIGGLER_ENABLED) != 0;
    uint8_t distance = p->motion[3] ? (uint8_t)p->motion[3] : 1; // Older S3s omit distance.
    if (j->session == p->session && j->sequence) {
        if (j->sequence == p->sequence)
            return j->enabled == enabled && j->interval_seconds == p->motion[0] && j->distance == distance && j->mode == mode;
        if (!kms_newer(p->sequence, j->sequence)) return false;
    }
    if (j->enabled != enabled || j->interval_seconds != p->motion[0] || j->distance != distance || j->mode != mode) {
        j->changed = now; j->running = false; j->remaining = 0;
    }
    if ((!j->enabled && enabled) || j->mode != mode) { j->right = false; j->phase = 0; }
    if (!j->random) { j->random = p->receiver ^ p->session; if (!j->random) j->random = 0x6d2b79f5u; }
    j->session = p->session; j->sequence = p->sequence;
    j->enabled = enabled; j->interval_seconds = (uint16_t)p->motion[0]; j->distance = distance; j->mode = (uint8_t)mode;
    return true;
}
bool kms_jiggler_motion(const kms_jiggler_t *j, uint32_t now, kms_jiggler_motion_t *out)
{
    *out = (kms_jiggler_motion_t){0};
    uint32_t wait = j->mode == KMS_JIGGLER_RANDOM && j->running ? KMS_JIGGLER_RANDOM_PERIOD_MS : (uint32_t)j->interval_seconds * 1000;
    if (!j->enabled || !kms_time_elapsed(now, j->changed, wait)) return false;
    if (j->mode == KMS_JIGGLER_LEFT_RIGHT) out->x = j->right ? j->distance : -(int32_t)j->distance;
    else if (j->mode == KMS_JIGGLER_FOUR_WAY) {
        static const int8_t directions[4][2] = {{0,-1}, {0,1}, {1,0}, {-1,0}};
        out->x = directions[j->phase][0] * j->distance; out->y = directions[j->phase][1] * j->distance;
    } else {
        static const int8_t directions[8][2] = {{1,0}, {1,1}, {0,1}, {-1,1}, {-1,0}, {-1,-1}, {0,-1}, {1,-1}};
        unsigned phase = j->remaining ? j->phase : random_phase(j, random_next(j->random));
        out->x = directions[phase][0] * j->distance; out->y = directions[phase][1] * j->distance;
    }
    return true;
}
void kms_jiggler_moved(kms_jiggler_t *j, uint32_t now)
{
    if (j->mode == KMS_JIGGLER_LEFT_RIGHT) j->right = !j->right;
    else if (j->mode == KMS_JIGGLER_FOUR_WAY) j->phase = (uint8_t)((j->phase + 1) % 4);
    else {
        if (!j->remaining) {
            j->random = random_next(j->random);
            j->phase = (uint8_t)random_phase(j, j->random);
            j->remaining = (uint8_t)(10 + (j->random >> 8) % 46);
        }
        --j->remaining; j->running = true;
    }
    j->changed = now;
}
void kms_jiggler_activity(kms_jiggler_t *j, uint32_t now)
{ j->changed = now; j->running = false; j->remaining = 0; }

void kms_jiggler_sync_request(kms_jiggler_sync_t *s, bool enabled, uint16_t seconds, uint8_t distance, unsigned mode)
{
    s->enabled = enabled; s->interval_seconds = seconds; s->distance = distance; s->mode = (uint8_t)mode;
    if (!++s->sequence) ++s->sequence;
    s->pending = true; s->sent = false;
}
void kms_jiggler_sync_status(kms_jiggler_sync_t *s, const kms_packet_t *p, uint32_t master, uint16_t seconds, uint8_t distance)
{
    if (p->type != KMS_STATUS || !p->session) return;
    if (!(p->flags & KMS_FLAG_JIGGLER_SUPPORTED)) { s->known = false; return; }
    bool enabled = (p->flags & KMS_FLAG_JIGGLER_ENABLED) != 0;
    bool distance_supported = (p->flags & KMS_FLAG_JIGGLER_DISTANCE) != 0;
    bool modes_supported = (p->flags & KMS_FLAG_JIGGLER_MODES) != 0;
    unsigned mode = modes_supported ? (p->flags & KMS_JIGGLER_MODE_MASK) >> KMS_JIGGLER_MODE_SHIFT : KMS_JIGGLER_LEFT_RIGHT;
    uint8_t reported_distance = distance_supported ? (uint8_t)p->motion[3] : 1;
    if (!p->motion[0] || p->motion[0] > KMS_JIGGLER_MAX_SECONDS || !reported_distance ||
        (distance_supported && p->motion[3] > KMS_JIGGLER_MAX_DISTANCE) || mode >= KMS_JIGGLER_MODE_COUNT) return;
    if (!distance_supported) distance = 1;
    if (!s->known || s->boot != p->session) {
        *s = (kms_jiggler_sync_t){.known = true, .boot = p->session, .enabled = enabled,
            .interval_seconds = (uint16_t)p->motion[0], .distance = reported_distance, .distance_supported = distance_supported,
            .mode = (uint8_t)mode, .modes_supported = modes_supported,
            .sequence = p->motion[1] == master ? p->motion[2] : 0};
    }
    // Delayed status must not undo a newer request or its confirmation.
    if (s->sequence && (p->motion[1] != master || kms_newer(s->sequence, p->motion[2]))) return;
    if (s->pending) {
        if (p->motion[1] == master && p->motion[2] == s->sequence &&
            enabled == s->enabled && p->motion[0] == s->interval_seconds && reported_distance == s->distance && mode == s->mode) s->pending = false;
    } else {
        s->enabled = enabled; s->mode = (uint8_t)mode;
        if (p->motion[0] != seconds || reported_distance != distance) kms_jiggler_sync_request(s, enabled, seconds, distance, mode);
    }
}
bool kms_jiggler_sync_packet(const kms_jiggler_sync_t *s, uint32_t master, unsigned slot, uint32_t now, kms_packet_t *p)
{
    if (!s->known || !s->pending || (s->sent && !kms_time_elapsed(now, s->sent_at, KMS_STATUS_MS))) return false;
    *p = (kms_packet_t){.type = KMS_JIGGLER, .slot = (uint8_t)slot, .session = master,
        .receiver = s->boot, .sequence = s->sequence, .flags = (s->enabled ? KMS_FLAG_JIGGLER_ENABLED : 0) | (s->mode << KMS_JIGGLER_MODE_SHIFT),
        .motion = {s->interval_seconds, 0, 0, s->distance}};
    return true;
}

unsigned kms_jiggler_indicator(uint32_t now, unsigned normal_color)
{
    if (now % 5000u >= 150u) return normal_color;
    return normal_color == KMS_COLOR_OFF ? KMS_COLOR_BLUE : KMS_COLOR_OFF;
}
