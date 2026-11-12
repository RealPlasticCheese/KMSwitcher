#include "kms_jiggler.h"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks;
static kms_packet_t report(const kms_jiggler_t *j, uint32_t boot)
{
    return (kms_packet_t){.type = KMS_STATUS, .session = boot,
        .flags = KMS_FLAG_JIGGLER_SUPPORTED | KMS_FLAG_JIGGLER_DISTANCE | KMS_FLAG_JIGGLER_MODES |
            (j->enabled ? KMS_FLAG_JIGGLER_ENABLED : 0) | (j->mode << KMS_JIGGLER_MODE_SHIFT),
        .motion = {j->interval_seconds ? j->interval_seconds : 1, j->session, j->sequence, j->distance ? j->distance : 3}};
}
__declspec(dllexport) int kms_run_jiggler_modes_tests(void)
{
    kms_jiggler_t j = {0}; kms_jiggler_motion_t move, retry;
    kms_packet_t p = {.type = KMS_JIGGLER, .session = 42, .receiver = 123, .sequence = 1,
        .flags = KMS_FLAG_JIGGLER_ENABLED | (KMS_JIGGLER_FOUR_WAY << KMS_JIGGLER_MODE_SHIFT), .motion = {1, 0, 0, 3}};
    CHECK(kms_jiggler_configure(&j, &p, 0));
    static const int32_t expected[4][2] = {{0,-3}, {0,3}, {3,0}, {-3,0}};
    for (unsigned i = 0; i < 12; ++i) {
        uint32_t now = (i + 1) * 1000;
        CHECK(!kms_jiggler_motion(&j, now - 1, &move));
        CHECK(kms_jiggler_motion(&j, now, &move));
        CHECK(move.x == expected[i % 4][0] && move.y == expected[i % 4][1]);
        CHECK(kms_jiggler_motion(&j, now + 1, &retry) && retry.x == move.x && retry.y == move.y);
        kms_jiggler_moved(&j, now);
    }
    ++p.sequence; p.flags = KMS_FLAG_JIGGLER_ENABLED | (KMS_JIGGLER_RANDOM << KMS_JIGGLER_MODE_SHIFT);
    CHECK(kms_jiggler_configure(&j, &p, 20000));
    unsigned directions = 0, run = 0, changes = 0; kms_jiggler_motion_t previous = {0};
    for (unsigned i = 0; i < 1000; ++i) {
        uint32_t now = 21000 + i * KMS_JIGGLER_RANDOM_PERIOD_MS;
        CHECK(!kms_jiggler_motion(&j, now - 1, &move));
        CHECK(kms_jiggler_motion(&j, now, &move));
        CHECK((move.x == -3 || move.x == 0 || move.x == 3) && (move.y == -3 || move.y == 0 || move.y == 3) && (move.x || move.y));
        CHECK(kms_jiggler_motion(&j, now + 1, &retry) && retry.x == move.x && retry.y == move.y);
        for (unsigned d = 0; d < 8; ++d) {
            static const int32_t vectors[8][2] = {{3,0}, {3,3}, {0,3}, {-3,3}, {-3,0}, {-3,-3}, {0,-3}, {3,-3}};
            if (move.x == vectors[d][0] && move.y == vectors[d][1]) directions |= 1u << d;
        }
        if (i && (move.x != previous.x || move.y != previous.y)) { CHECK(run >= 10 && run <= 55); run = 0; ++changes; }
        ++run; previous = move;
        if (i == 500) { CHECK(kms_jiggler_configure(&j, &p, now) && j.changed == now - 20); }
        kms_jiggler_moved(&j, now);
    }
    CHECK(directions == 255 && changes > 20);
    kms_jiggler_activity(&j, 50000);
    CHECK(j.enabled && !j.running && !kms_jiggler_motion(&j, 50999, &move));
    CHECK(kms_jiggler_motion(&j, 51000, &move)); kms_jiggler_moved(&j, 51000);
    CHECK(!kms_jiggler_motion(&j, 51019, &move) && kms_jiggler_motion(&j, 51020, &move));
    CHECK(kms_jiggler_motion(&j, 900000, &move)); kms_jiggler_moved(&j, 900000);
    CHECK(!kms_jiggler_motion(&j, 900001, &move)); // No catch-up burst after a busy/suspended host.
    ++p.sequence; p.flags = KMS_FLAG_JIGGLER_ENABLED | (3u << KMS_JIGGLER_MODE_SHIFT);
    CHECK(!kms_jiggler_configure(&j, &p, 900001) && j.mode == KMS_JIGGLER_RANDOM);
    p.flags = KMS_JIGGLER_RANDOM << KMS_JIGGLER_MODE_SHIFT;
    CHECK(kms_jiggler_configure(&j, &p, 900010) && !j.enabled && !kms_jiggler_motion(&j, 999999, &move));
    ++p.sequence; p.flags = KMS_FLAG_JIGGLER_ENABLED;
    CHECK(kms_jiggler_configure(&j, &p, 1000000) && j.mode == KMS_JIGGLER_LEFT_RIGHT); // Legacy S3.
    CHECK(kms_jiggler_motion(&j, 1001000, &move) && move.x == -3 && !move.y);

    kms_jiggler_sync_t receivers[2] = {0}; kms_jiggler_t actual[2] = {0};
    kms_packet_t status = report(&actual[0], 11), command;
    kms_jiggler_sync_status(&receivers[0], &status, 42, 1, 3);
    status = report(&actual[1], 12); kms_jiggler_sync_status(&receivers[1], &status, 42, 1, 3);
    kms_jiggler_sync_request(&receivers[0], false, 1, 3, KMS_JIGGLER_FOUR_WAY);
    CHECK(kms_jiggler_sync_packet(&receivers[0], 42, 1, 0, &command));
    CHECK(kms_jiggler_configure(&actual[0], &command, 0) && !actual[0].enabled && actual[0].mode == KMS_JIGGLER_FOUR_WAY);
    status = report(&actual[0], 11); status.flags ^= 1u << KMS_JIGGLER_MODE_SHIFT;
    kms_jiggler_sync_status(&receivers[0], &status, 42, 1, 3); CHECK(receivers[0].pending);
    status = report(&actual[0], 11); kms_jiggler_sync_status(&receivers[0], &status, 42, 1, 3);
    CHECK(!receivers[0].pending && receivers[0].mode == KMS_JIGGLER_FOUR_WAY && !receivers[0].enabled);
    CHECK(receivers[1].mode == KMS_JIGGLER_LEFT_RIGHT && !receivers[1].pending);
    kms_jiggler_sync_request(&receivers[0], true, 2, 4, receivers[0].mode);
    CHECK(kms_jiggler_sync_packet(&receivers[0], 42, 1, 1, &command));
    CHECK(kms_jiggler_configure(&actual[0], &command, 1) && actual[0].mode == KMS_JIGGLER_FOUR_WAY);
    actual[0] = (kms_jiggler_t){0}; status = report(&actual[0], 13);
    kms_jiggler_sync_status(&receivers[0], &status, 42, 1, 3);
    CHECK(!receivers[0].enabled && receivers[0].mode == KMS_JIGGLER_LEFT_RIGHT && !receivers[0].pending);
    status.flags &= (uint8_t)~KMS_FLAG_JIGGLER_MODES; receivers[1] = (kms_jiggler_sync_t){0};
    kms_jiggler_sync_status(&receivers[1], &status, 42, 1, 3);
    CHECK(receivers[1].known && !receivers[1].modes_supported && receivers[1].mode == KMS_JIGGLER_LEFT_RIGHT);
    return 0;
}
__declspec(dllexport) int kms_jiggler_modes_test_count(void) { return checks; }
