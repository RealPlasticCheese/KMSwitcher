#include "kms_test.h"
#ifdef CONFIG_KMS_TEST_SUPPORT
#include "kms_diagnostics.h"
#include "kms_runtime.h"
#include "kms_test_mode.h"
#include "kms_remote.h"
#include "kms_management.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#ifdef CONFIG_SPIRAM
#include "esp_heap_caps.h"
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Explicit byte encoding: independent of C padding, endianness and SDK versions.
enum { RECORDS_PER_CHUNK = 64, RECORD_CHUNKS = KMS_TEST_RECORDS / RECORDS_PER_CHUNK };
_Static_assert(KMS_TEST_RECORDS % RECORDS_PER_CHUNK == 0, "Trace chunks must cover the full ring");
static uint8_t *records[RECORD_CHUNKS];
static uint32_t boot_id, run_id, next_record, overwritten, page_index;
static uint32_t drop_interval, drop_count;
static uint32_t summary[23];
static uint8_t source_keys[2][34], reference_keys[34], routed_keys[34], completed_keys[34];
static bool enabled, frozen, is_master, allocation_failed;
static bool usb_unavailable;
static bool restart_requested;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static void *allocate_chunk(void)
{
    size_t bytes = RECORDS_PER_CHUNK * KMS_TEST_RECORD_BYTES;
#ifdef CONFIG_SPIRAM
    if (is_master) {
        // Normal malloc stays internal on the S3; USB can fragment that heap.
        void *buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer) return buffer;
    }
#endif
    return malloc(bytes);
}
static bool allocate_records(void)
{
    // The S2 can have enough total RAM without one contiguous 23 KB block.
    // Keep the full trace window using smaller, independently owned chunks.
    uint8_t *chunks[RECORD_CHUNKS] = {0};
    for (unsigned i = 0; i < RECORD_CHUNKS; ++i) {
        chunks[i] = allocate_chunk();
        if (!chunks[i]) {
            for (unsigned n = 0; n < i; ++n) free(chunks[n]);
            return false;
        }
    }
    memcpy(records, chunks, sizeof(records));
    return true;
}
static uint8_t *record_at(uint32_t index)
{
    unsigned slot = index % KMS_TEST_RECORDS;
    return records[slot / RECORDS_PER_CHUNK] + (slot % RECORDS_PER_CHUNK) * KMS_TEST_RECORD_BYTES;
}
void kms_test_init(uint32_t boot, bool master)
{
    boot_id = boot; is_master = master; restart_requested = false;
    // No capture buffer is reserved during everyday operation or a reboot.
    if (master) printf("KMSBENCH BOOT %08" PRIx32 "\n", boot);
}
static void stop_capture(void)
{
    uint8_t *old_records[RECORD_CHUNKS];
    portENTER_CRITICAL(&lock);
    memcpy(old_records, records, sizeof(records)); memset(records, 0, sizeof(records));
    enabled = frozen = usb_unavailable = allocation_failed = false;
    drop_interval = drop_count = next_record = overwritten = page_index = run_id = 0;
    portEXIT_CRITICAL(&lock);
    for (unsigned i = 0; i < RECORD_CHUNKS; ++i) free(old_records[i]);
}
bool kms_test_usb_available(void)
{
    portENTER_CRITICAL(&lock); bool available = !enabled || !usb_unavailable; portEXIT_CRITICAL(&lock);
    return available;
}
bool kms_test_take_restart(void)
{
    portENTER_CRITICAL(&lock);
    bool requested = restart_requested;
    restart_requested = false;
    portEXIT_CRITICAL(&lock);
    return requested;
}
bool kms_test_drop(unsigned packet_type)
{
    bool dropped = false;
    portENTER_CRITICAL(&lock);
    if (enabled && drop_interval && (is_master ? (packet_type == KMS_INPUT || packet_type == KMS_INPUT_BATCH) : packet_type == KMS_ACK))
        dropped = ++drop_count % drop_interval == 0;
    portEXIT_CRITICAL(&lock);
    if (dropped) kms_test_record(KMS_T_INJECT, packet_type, 0, 0, NULL, 0);
    return dropped;
}
static void keyboard_digest(uint8_t *previous, const uint8_t *report, unsigned count_word, unsigned crc_word)
{
    if (memcmp(previous, report, 34)) {
        memcpy(previous, report, 34); ++summary[count_word];
        summary[crc_word] = kms_crc32_extend(summary[crc_word], report, 34);
    }
}
static void summarize(unsigned stage, uint32_t a, const uint8_t *data, size_t bytes)
{
    static const uint8_t words[] = {0,13,14,15,16,17,18,19,20,21,22,0,11,0,12};
    if (stage < sizeof(words) && words[stage]) ++summary[words[stage]];
    if (stage == KMS_T_ROUTED && bytes == 60) {
        uint8_t report[34] = {data[32], 0}; memcpy(report + 2, data, 32);
        keyboard_digest(routed_keys, report, 1, 2);
    } else if (stage == KMS_T_USB_COMPLETE && a == 0 && bytes == 34) {
        keyboard_digest(completed_keys, data, 5, 6);
    } else if (stage == KMS_T_RAW && bytes == 55 && data[0] == 1 &&
               data[44] == 'K' && data[45] == 'M' && kms_get_u32(data + 47) == run_id &&
               data[46] >= 1 && data[46] <= 2) {
        unsigned source = data[46] - 1;
        if (kms_get_u32(data + 51)) {
            ++summary[7 + source * 2];
            summary[8 + source * 2] = kms_crc32_extend(summary[8 + source * 2], data + 1, 54);
        }
        source_keys[source][0] = data[1]; memcpy(source_keys[source] + 2, data + 2, 32);
        uint8_t merged[34];
        for (unsigned i = 0; i < sizeof(merged); ++i) merged[i] = source_keys[0][i] | source_keys[1][i];
        keyboard_digest(reference_keys, merged, 3, 4);
    }
}
void kms_test_record(unsigned stage, uint32_t a, uint32_t b, uint32_t c,
                     const void *data, size_t bytes)
{
    if (!data) bytes = 0;
    if (bytes > 64) bytes = 64;
    portENTER_CRITICAL(&lock);
    if (enabled) {
        summarize(stage, a, data, bytes);
    }
    if (enabled && !frozen && records[0]) {
        uint32_t id = next_record++;
        uint8_t *p = record_at(id);
        memset(p, 0, KMS_TEST_RECORD_BYTES);
        kms_put_u32(p, id); kms_put_u32(p + 4, (uint32_t)esp_timer_get_time());
        kms_put_u32(p + 8, stage); kms_put_u32(p + 12, a);
        kms_put_u32(p + 16, b); kms_put_u32(p + 20, c);
        kms_put_u32(p + 24, (uint32_t)bytes);
        if (bytes && data) memcpy(p + 28, data, bytes);
        if (id >= KMS_TEST_RECORDS) ++overwritten;
        // Latch the first fault and its preceding window on the device task,
        // before recovery or a slow host can overwrite the evidence. Summaries
        // above keep running, as do input, recovery and fault injection.
        if (stage == KMS_T_FAULT && a != KMS_EVENT_BOOT &&
            a != KMS_EVENT_RECOVERED && a != KMS_EVENT_SELECTED) frozen = true;
    }
    portEXIT_CRITICAL(&lock);
}
void kms_test_sample(unsigned stage, uint32_t a, const kms_sample_t *s)
{
    portENTER_CRITICAL(&lock); bool capture = enabled; portEXIT_CRITICAL(&lock);
    if (!capture) return;
    uint8_t data[64] = {0};
    memcpy(data, s->state.keys, KMS_KEY_BYTES); data[32] = s->state.modifiers;
    data[33] = (uint8_t)s->state.buttons; data[34] = (uint8_t)(s->state.buttons >> 8);
    for (unsigned n = 0; n < KMS_CONSUMERS; ++n) {
        data[35 + n * 2] = (uint8_t)s->state.consumer[n];
        data[36 + n * 2] = (uint8_t)(s->state.consumer[n] >> 8);
    }
    data[43] = s->state.system;
    kms_put_u32(data + 44, (uint32_t)s->x); kms_put_u32(data + 48, (uint32_t)s->y);
    kms_put_u32(data + 52, (uint32_t)s->wheel); kms_put_u32(data + 56, (uint32_t)s->pan);
    kms_test_record(stage, a, 0, 0, data, 60);
}
void kms_test_command(const uint8_t *data, size_t size)
{
    if (!data || size < 8 || memcmp(data, "KMT1", 4)) return;
    uint32_t op = kms_get_u32(data + 4);
    if (op == KMS_TEST_RESTART) {
        if (!is_master && size >= 12 && kms_get_u32(data + 8) == KMS_TEST_RESTART_KEY) {
            portENTER_CRITICAL(&lock); restart_requested = true; portEXIT_CRITICAL(&lock);
        }
        return;
    }
    if (op == 1 && size >= 12) {
        // Receiver feature callbacks only change RAM. The master's explicit
        // session command commits the restart guard before any trace is armed.
        if (is_master && (!kms_test_mode_active() || !kms_test_mode_set(true))) return;
        if (!is_master) kms_test_mode_set(true);
        if (!records[0] && !allocate_records()) { allocation_failed = true; return; }
        allocation_failed = false;
    } else if (op == 3) {
        stop_capture();
        if (!is_master) kms_test_mode_set(false);
        return;
    }
    portENTER_CRITICAL(&lock);
    if (op == 1 && size >= 12) {
        run_id = kms_get_u32(data + 8); next_record = overwritten = page_index = 0;
        frozen = false; enabled = true; usb_unavailable = false; drop_interval = drop_count = 0;
        memset(summary, 0, sizeof(summary)); summary[0] = 0x314d5553u; // SUM1
        memset(source_keys, 0, sizeof(source_keys)); memset(reference_keys, 0, sizeof(reference_keys));
        memset(routed_keys, 0, sizeof(routed_keys)); memset(completed_keys, 0, sizeof(completed_keys));
    } else if (op == 2) frozen = true;
    else if (op == 4 && frozen && size >= 12) page_index = kms_get_u32(data + 8);
    else if (op == 5 && enabled && size >= 12) { drop_interval = kms_get_u32(data + 8); drop_count = 0; }
    else if (op == 6 && enabled && size >= 12) usb_unavailable = kms_get_u32(data + 8) != 0;
    portEXIT_CRITICAL(&lock);
}
size_t kms_test_page(uint8_t *out, size_t cap)
{
    if (cap < KMS_TEST_PAGE) return 0;
    memset(out, 0, KMS_TEST_PAGE); memcpy(out, "KMT1", 4);
    portENTER_CRITICAL(&lock);
    kms_put_u32(out + 4, boot_id); kms_put_u32(out + 8, run_id);
    kms_put_u32(out + 12, next_record); kms_put_u32(out + 16, overwritten);
    kms_put_u32(out + 20, (enabled ? 1u : 0u) | (frozen ? 2u : 0u) |
                        (allocation_failed ? 4u : 0u) | (kms_test_mode_active() ? 8u : 0u) |
                        (is_master ? 0u : KMS_TEST_CAN_RESTART));
    uint32_t first = next_record > KMS_TEST_RECORDS ? next_record - KMS_TEST_RECORDS : 0;
    uint32_t index = page_index == UINT32_MAX ? UINT32_MAX : first + page_index;
    kms_put_u32(out + 24, index);
    if (frozen && page_index == UINT32_MAX) {
        for (unsigned i = 0; i < 23; ++i) kms_put_u32(out + 28 + i * 4, summary[i]);
    } else if (records[0] && frozen && page_index < KMS_TEST_RECORDS && index >= first && index < next_record)
        memcpy(out + 28, record_at(index), KMS_TEST_RECORD_BYTES);
    portEXIT_CRITICAL(&lock);
    kms_put_u32(out + 124, kms_crc32(out, 124));
    return KMS_TEST_PAGE;
}
bool kms_test_console(const char *line)
{
    if (!strcmp(line, "test-mode") || !strcmp(line, "test-mode on") || !strcmp(line, "test-mode off")) {
        if (strcmp(line, "test-mode")) {
            if (kms_link_pairing() || kms_remote_master_updating()) {
                printf("KMSTEST ERROR pairing or firmware update in progress\n"); return true;
            }
            bool enter = !strcmp(line, "test-mode on");
            // Never switch destinations as a side effect of changing mode.
            kms_link_pause();
            if (!kms_test_mode_set(enter)) {
                printf("KMSTEST ERROR cannot save mode; routing remains paused\n"); return true;
            }
            if (!enter) stop_capture();
        }
        printf("KMSTEST MODE %s\n", kms_test_mode_active() ? "test" : "everyday"); return true;
    }
    if (strncmp(line, "bench ", 6)) return false;
    unsigned value; int consumed = 0;
    uint8_t command[12] = {'K','M','T','1'};
    if (!strcmp(line, "bench info")) {
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
        printf("KMSBENCH INFO 1 %s %02X%02X%02X%02X%02X%02X %08" PRIx32 " %u\n",
               is_master ? "master" : "receiver", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], boot_id, kms_link_selected());
    } else if (sscanf(line, "bench arm %u%n", &value, &consumed) == 1 && !line[consumed]) {
        kms_put_u32(command + 4, 1); kms_put_u32(command + 8, value); kms_test_command(command, sizeof(command));
        if (enabled && run_id == value) printf("KMSBENCH ARMED %u\n", value);
        else printf("KMSBENCH ERROR enable test-mode on first; trace RAM must be available\n");
    } else if (!strcmp(line, "bench freeze") || !strcmp(line, "bench off")) {
        kms_put_u32(command + 4, line[6] == 'f' ? 2 : 3); kms_test_command(command, sizeof(command));
        printf("KMSBENCH OK\n");
    } else if (sscanf(line, "bench page %u%n", &value, &consumed) == 1 && !line[consumed]) {
        kms_put_u32(command + 4, 4); kms_put_u32(command + 8, value); kms_test_command(command, sizeof(command));
        uint8_t page[KMS_TEST_PAGE]; char hex[KMS_TEST_PAGE * 2 + 1];
        kms_test_page(page, sizeof(page)); kms_hex_encode(page, sizeof(page), hex);
        printf("KMSBENCH PAGE %s\n", hex);
    } else if (sscanf(line, "bench drop %u%n", &value, &consumed) == 1 && !line[consumed]) {
        kms_put_u32(command + 4, 5); kms_put_u32(command + 8, value); kms_test_command(command, sizeof(command));
        printf("KMSBENCH DROP %u\n", value);
    } else printf("KMSBENCH ERROR invalid command\n");
    return true;
}
#endif
