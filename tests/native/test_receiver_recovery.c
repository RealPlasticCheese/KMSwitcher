#include "kms_receiver_recovery.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks;
static void faults(kms_receiver_recovery_t *p, bool active, uint32_t at, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        kms_receiver_recovery_fault(p, 22, active, KMS_RECOVERY_MAC, at + i * 500u);
}
__declspec(dllexport) int kms_run_receiver_recovery_tests(void)
{
    checks = 0; kms_receiver_recovery_t p = {0}; kms_receiver_recovery_boot(&p, 22, 100);
    faults(&p, false, 6000, 3); CHECK(!kms_receiver_recovery_due(&p, false, 8000));
    faults(&p, false, 8000, 1); CHECK(kms_receiver_recovery_due(&p, false, 8000));
    CHECK(!kms_receiver_recovery_due(&p, true, 8000));
    kms_receiver_recovery_begin(&p, 8000);
    CHECK(p.waiting && p.attempts == 1 && p.next_at == 68000 && p.level == 1);
    CHECK(!kms_receiver_recovery_confirm(&p, 22, true, 9000));
    CHECK(!kms_receiver_recovery_confirm(&p, 23, false, 9000));
    CHECK(kms_receiver_recovery_confirm(&p, 23, true, 9000) && p.attempts == 1 && p.level == 1);
    kms_receiver_recovery_boot(&p, 22, 10000);
    faults(&p, true, 20000, 11); CHECK(!kms_receiver_recovery_due(&p, true, 25500));
    faults(&p, true, 25500, 1); CHECK(!kms_receiver_recovery_due(&p, true, 25500));
    faults(&p, true, 70000, 12); CHECK(kms_receiver_recovery_due(&p, true, 75500));
    kms_receiver_recovery_begin(&p, 75500); CHECK(p.attempts == 2 && p.next_at == 195500);
    CHECK(!kms_receiver_recovery_expired(&p, 95499)); CHECK(kms_receiver_recovery_expired(&p, 95500));
    faults(&p, false, 200000, 5); CHECK(kms_receiver_recovery_due(&p, false, 202000));
    kms_receiver_recovery_begin(&p, 202000); CHECK(p.attempts == 3 && p.next_at == 442000);
    p.waiting = false; faults(&p, false, 500000, 5); CHECK(!kms_receiver_recovery_due(&p, false, 502000));
    p.attempt_at[0] = 0; p.attempt_at[1] = 3590000; p.attempt_at[2] = 3599000;
    faults(&p, false, 3601000, 5); CHECK(kms_receiver_recovery_due(&p, false, 3603000));
    kms_receiver_recovery_begin(&p, 3603000); CHECK(p.attempts == 3);
    CHECK(p.attempt_at[0] == 3590000 && p.attempt_at[2] == 3603000);
    p.waiting = false; p.cooldown = false;
    faults(&p, false, 3700000, 5); CHECK(!kms_receiver_recovery_due(&p, false, 3702000));
    p = (kms_receiver_recovery_t){0}; kms_receiver_recovery_boot(&p, 22, 100);
    for (unsigned i = 0; i < 100; ++i) kms_receiver_recovery_fault(&p, 22, false, KMS_RECOVERY_ACK, 1000 + i);
    CHECK(p.faults == 1 && !kms_receiver_recovery_due(&p, false, 3100));
    p.fault_seen = false;
    for (unsigned i = 0; i < 4; ++i) kms_receiver_recovery_fault(&p, 22, false, KMS_RECOVERY_MAC, 6000 + i * 250);
    CHECK(p.faults == 4 && !kms_receiver_recovery_due(&p, false, 11000));
    faults(&p, false, 20000, 3); CHECK(!kms_receiver_recovery_due(&p, false, 22000));
    faults(&p, false, 40000, 3); CHECK(!kms_receiver_recovery_due(&p, false, 42000));
    p.level = 8; faults(&p, false, 42000, 1); CHECK(kms_receiver_recovery_due(&p, false, 42000));
    kms_receiver_recovery_begin(&p, 42000); CHECK(p.next_at == 42000 + KMS_RECOVERY_BACKOFF_MAX_MS);
    p.waiting = false; p.healthy_at = 50000;
    kms_receiver_recovery_healthy(&p, 50000 + KMS_RECOVERY_HEALTHY_MS - 1); CHECK(p.level == 8);
    kms_receiver_recovery_healthy(&p, 50000 + KMS_RECOVERY_HEALTHY_MS); CHECK(!p.level && p.attempts == 1);
    p = (kms_receiver_recovery_t){0}; kms_receiver_recovery_boot(&p, 22, 0xffff0000u);
    faults(&p, false, 0xfffffff0u, 5); CHECK(kms_receiver_recovery_due(&p, false, 0xfffffff0u + 2000u));
    kms_receiver_recovery_begin(&p, 0xfffffff0u + 2000u);
    CHECK(!kms_receiver_recovery_expired(&p, 0xfffffff0u + 21999u));
    CHECK(kms_receiver_recovery_expired(&p, 0xfffffff0u + 22000u));
    kms_recovery_budget_t budgets[KMS_SLOTS] = {0}, decoded[KMS_SLOTS];
    budgets[1] = (kms_recovery_budget_t){.mac = {2, 3, 4, 5, 6, 7}, .attempts = 3, .level = 4, .boot = 22, .reasons = 7};
    uint8_t data[KMS_RECOVERY_STORE_BYTES]; bool enabled = false;
    CHECK(kms_recovery_store_encode(true, budgets, data));
    CHECK(kms_recovery_store_decode(&enabled, decoded, data, sizeof(data)) && enabled && !memcmp(budgets, decoded, sizeof(budgets)));
    for (unsigned i = 0; i < sizeof(data); ++i) {
        data[i] ^= 1; CHECK(!kms_recovery_store_decode(&enabled, decoded, data, sizeof(data))); data[i] ^= 1;
    }
    CHECK(!kms_recovery_store_decode(&enabled, decoded, data, sizeof(data) - 1));
    p = (kms_receiver_recovery_t){0}; kms_receiver_recovery_restore(&p, &budgets[1], 1000);
    kms_receiver_recovery_boot(&p, 22, 1000); faults(&p, false, 1000000, 5);
    CHECK(!kms_receiver_recovery_due(&p, false, 1002000));
    faults(&p, false, 3602000, 5); CHECK(kms_receiver_recovery_due(&p, false, 3604000));
    return 0;
}
__declspec(dllexport) int kms_receiver_recovery_test_count(void) { return (int)checks; }
