#include "kms_slave_snapshot.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks;
static kms_slave_snapshot_t snapshot;
_Static_assert(sizeof(kms_slave_snapshot_t) < 13 * 1024, "Snapshot metadata must remain below 13 KiB");
static kms_slave_history_t live;
static uint8_t full[KMS_SLAVE_HISTORY_BYTES], chunk[KMS_M_DATA + 2];
static bool contains(const char *needle, size_t length)
{
    size_t size = 0; while (needle[size]) ++size;
    for (size_t at = 0; at + size <= length; ++at)
        if (!memcmp(full + at, needle, size)) return true;
    return false;
}
__declspec(dllexport) int kms_run_snapshot_tests(void)
{
    checks = 0; memset(&snapshot, 0, sizeof(snapshot));
    kms_slave_history_init(&live, 0, 55, 1);
    kms_slave_history_event(&live, 100, KMS_S_USB_STALL, 42);
    snapshot.history = live;
    memcpy(snapshot.identity.git, "test", 5); memcpy(snapshot.identity.utc, "UTC", 4);
    snapshot.identity.image_bytes = 123;
    CHECK(kms_slave_snapshot_prepare(&snapshot));
    CHECK(snapshot.size < sizeof(full));
    CHECK(kms_slave_snapshot_read(&snapshot, 0, full, sizeof(full)) == snapshot.size);
    CHECK(contains("git=test utc=UTC sha256=0000000000000000", snapshot.size));
    CHECK(contains("bytes=123\nretention events=1/256 overwritten=0 checkpoints=0/24 overwritten=0\n", snapshot.size));
    CHECK(contains("event seq=1 boot=00000037 first_ms=100 last_ms=100 code=5 detail=42 repeats=1 name=USB stalled\n", snapshot.size));
    CHECK(snapshot.crc == kms_crc32(full, snapshot.size));
    uint32_t size = snapshot.size, crc = snapshot.crc;
    kms_slave_history_clear(&live); kms_slave_history_event(&live, 200, KMS_S_BOOT, 1);
    CHECK(snapshot.size == size && snapshot.crc == crc);
    CHECK(kms_slave_snapshot_read(&snapshot, size, chunk, sizeof(chunk)) == 0);
    CHECK(kms_slave_snapshot_read(&snapshot, UINT32_MAX, chunk, sizeof(chunk)) == 0);
    CHECK(kms_slave_snapshot_read(&snapshot, 0, chunk, 0) == 0);
    CHECK(kms_slave_snapshot_read(&snapshot, 0, 0, 1) == 0);
    snapshot.history.journal.version = 1;
    CHECK(!kms_slave_snapshot_prepare(&snapshot));
    CHECK(kms_slave_snapshot_read(&snapshot, 0, chunk, sizeof(chunk)) == 0);

    // Fill every record and checkpoint with maximum-width values. This export
    // exceeds the former 64 KiB host limit and must still use small chunk buffers.
    kms_slave_history_init(&live, 0, UINT32_MAX, UINT32_MAX);
    for (unsigned n = 0; n < KMS_SLAVE_HISTORY_EVENTS; ++n)
        kms_slave_history_event(&live, UINT32_MAX, KMS_S_STORAGE_ERROR, n);
    live.live.sequence = UINT32_MAX; live.live.uptime_ms = ~(uint64_t)0;
    live.live.rssi_last = live.live.rssi_mean = (-2147483647 - 1);
    for (unsigned n = 0; n < KMS_S_METRIC_COUNT; ++n) live.live.metrics[n] = UINT32_MAX;
    for (unsigned n = 0; n < KMS_S_EVENT_COUNT; ++n) live.live.totals[n] = UINT32_MAX;
    live.journal.stats_count = KMS_SLAVE_HISTORY_STATS;
    for (unsigned n = 0; n < KMS_SLAVE_HISTORY_STATS; ++n) live.journal.snapshots[n] = live.live;
    for (unsigned n = 0; n < KMS_SLAVE_HISTORY_EVENTS; ++n) {
        live.journal.events[n].sequence = UINT32_MAX;
        live.journal.events[n].detail = live.journal.events[n].repeats = UINT32_MAX;
    }
    snapshot.history = live;
    CHECK(kms_slave_snapshot_prepare(&snapshot));
    CHECK(snapshot.size > 65536 && snapshot.size <= KMS_SLAVE_HISTORY_BYTES);
    CHECK(kms_slave_snapshot_read(&snapshot, 0, full, sizeof(full)) == snapshot.size);
    CHECK(snapshot.crc == kms_crc32(full, snapshot.size));
    CHECK(contains("uptime_ms=18446744073709551615 rssi=-2147483648 mean_rssi=-2147483648", snapshot.size));
    for (uint32_t offset = 0; offset < snapshot.size; offset += KMS_M_DATA) {
        memset(chunk, 0xa5, sizeof(chunk));
        size_t count = snapshot.size - offset;
        if (count > KMS_M_DATA) count = KMS_M_DATA;
        CHECK(kms_slave_snapshot_read(&snapshot, offset, chunk + 1, KMS_M_DATA) == count);
        CHECK(!memcmp(chunk + 1, full + offset, count));
        CHECK(chunk[0] == 0xa5 && chunk[count + 1] == 0xa5);
        // Read the same bytes again after live history wraps; retries are immutable.
        kms_slave_history_event(&live, offset, KMS_S_BOOT, offset);
        CHECK(kms_slave_snapshot_read(&snapshot, offset, chunk + 1, count) == count);
        CHECK(!memcmp(chunk + 1, full + offset, count));
    }
    for (unsigned n = 1; n < snapshot.blocks; ++n) {
        uint32_t offset = snapshot.offsets[n] - 1;
        size_t count = kms_slave_snapshot_read(&snapshot, offset, chunk, sizeof(chunk));
        CHECK(count && !memcmp(chunk, full + offset, count));
    }
    for (unsigned n = 0; n < KMS_SLAVE_HISTORY_EVENTS; ++n) {
        snapshot.history.journal.events[n].event = KMS_S_RADIO_ERROR;
        snapshot.history.journal.events[n].detail = kms_slave_radio_detail(KMS_RF_MAC_FAILED, 131, 999999u);
    }
    snapshot.history.live.radio_failed = UINT32_MAX;
    CHECK(kms_slave_snapshot_prepare(&snapshot) && snapshot.size <= KMS_SLAVE_HISTORY_BYTES);
    CHECK(kms_slave_snapshot_read(&snapshot, 0, full, sizeof(full)) == snapshot.size);
    CHECK(contains("radio_reason=1 op=3 storage_busy=1 duration_ms=65535 name=radio fault", snapshot.size));
    CHECK(contains("radio_failed=4294967295", snapshot.size));
    return 0;
}
__declspec(dllexport) unsigned kms_snapshot_test_count(void) { return checks; }
__declspec(dllexport) unsigned kms_snapshot_ram_bytes(void) { return sizeof(snapshot); }
