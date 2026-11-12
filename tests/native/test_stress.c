// Real trace ring, wire CRC, HID parser and generator pattern; no hardware.
#include "kms_core.h"
#include "pattern.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "nvs.h"
static bool saved_mode, mode_present, fail_mode_read, fail_mode_write, fail_alloc;
static unsigned mode_writes, allocations, frees, pauses;
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle)
{ (void)ns; (void)mode; *handle = 1; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size)
{
    (void)handle; (void)key;
    if (fail_mode_read) return ESP_FAIL;
    if (!mode_present) return ESP_ERR_NVS_NOT_FOUND;
    *(uint8_t *)data = saved_mode ? 1 : 0; *size = 1; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    (void)handle; (void)key; (void)size;
    if (fail_mode_write) return ESP_FAIL;
    saved_mode = *(const uint8_t *)data != 0; mode_present = true; ++mode_writes; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return fail_mode_write ? ESP_FAIL : ESP_OK; }
#include "../../firmware/components/kms_runtime/test_mode.c"
static uint8_t trace_memory[4][64 * 92];
static bool trace_used[4];
static unsigned allocation_limit = 4;
static bool fail_psram;
static unsigned psram_attempts, internal_attempts, requested_caps;
static void *take_memory(size_t bytes)
{
    if (fail_alloc || bytes > sizeof(trace_memory[0])) return NULL;
    for (unsigned i = 0; i < allocation_limit; ++i) {
        if (!trace_used[i]) { trace_used[i] = true; ++allocations; return trace_memory[i]; }
    }
    return NULL;
}
void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    ++psram_attempts; requested_caps = caps;
    return fail_psram ? NULL : take_memory(bytes);
}
static void *fake_malloc(size_t bytes)
{ ++internal_attempts; return take_memory(bytes); }
static void fake_free(void *data)
{
    for (unsigned i = 0; i < 4; ++i) {
        if (data == trace_memory[i] && trace_used[i]) { trace_used[i] = false; ++frees; return; }
    }
}
#define malloc fake_malloc
#define free fake_free
static int fake_printf(const char *format, ...) { (void)format; return 0; }
static int fake_scan(const char *text, const char *format, ...) { (void)text; (void)format; return 0; }
static int fake_compare(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
static int fake_ncompare(const char *a, const char *b, size_t n)
{ for (size_t i = 0; i < n; ++i) { if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i]; } return 0; }
#define printf fake_printf
#define sscanf fake_scan
#define strcmp fake_compare
#define strncmp fake_ncompare
#include "../../firmware/components/kms_runtime/test_trace.c"
int64_t esp_timer_get_time(void) { return 123456; }
int esp_read_mac(uint8_t *mac, unsigned type) { (void)type; memset(mac, 1, 6); return 0; }
unsigned kms_link_selected(void) { return 4; }
void kms_link_pause(void) { ++pauses; }
bool kms_link_pairing(void) { return false; }
bool kms_remote_master_updating(void) { return false; }
static kms_hid_parser_t parser;
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static int first_fault_retention(bool master)
{
    uint8_t command[12] = {'K','M','T','1'}, page[KMS_TEST_PAGE];
    kms_test_mode_init(master); CHECK(kms_test_mode_set(true));
    kms_test_init(800 + (unsigned)master, master);
    kms_put_u32(command + 4, 1); kms_put_u32(command + 8, 100); kms_test_command(command, sizeof(command));
    for (unsigned i = 0; i < 600; ++i) kms_test_record(KMS_T_TX, i, 0, 0, NULL, 0);
    kms_test_record(KMS_T_FAULT, KMS_EVENT_BOOT, 0, 0, NULL, 0);
    kms_test_record(KMS_T_FAULT, KMS_EVENT_RECOVERED, 3, 10, NULL, 0);
    kms_test_record(KMS_T_FAULT, KMS_EVENT_SELECTED, 3, 0, NULL, 0);
    CHECK(!frozen && next_record == 603);
    kms_test_record(KMS_T_INJECT, KMS_ACK, 0, 0, NULL, 0);
    CHECK(!frozen); // Deliberately dropping a packet is not itself a fault.
    kms_test_record(KMS_T_FAULT, KMS_EVENT_ACK_TIMEOUT, 3, 21, NULL, 0);
    CHECK(frozen && enabled && next_record == 605 && overwritten == 349);
    for (unsigned i = 0; i < 5000; ++i) {
        kms_test_record(KMS_T_TX, i, 0, 0, NULL, 0);
        kms_test_record(KMS_T_FAULT, KMS_EVENT_LEASE_EXPIRED, 3, i, NULL, 0);
    }
    uint8_t keyboard[34] = {0}; keyboard[2] = 0x10;
    kms_test_record(KMS_T_USB_COMPLETE, 0, 0, 0, keyboard, sizeof(keyboard));
    CHECK(next_record == 605 && overwritten == 349 && summary[16] == 5600);
    CHECK(summary[11] == 5004 && summary[5] == 1); // Full-run summaries continue.
    CHECK(summary[6] == kms_crc32(keyboard, sizeof(keyboard)));
    kms_put_u32(command + 4, 4); kms_put_u32(command + 8, 0); kms_test_command(command, sizeof(command));
    kms_test_page(page, sizeof(page));
    CHECK(kms_get_u32(page + 24) == 349 && kms_get_u32(page + 28) == 349);
    CHECK(kms_get_u32(page + 40) == 349); // Oldest retained TX before the fault.
    kms_put_u32(command + 8, 255); kms_test_command(command, sizeof(command));
    kms_test_page(page, sizeof(page));
    CHECK(kms_get_u32(page + 28) == 604 && kms_get_u32(page + 36) == KMS_T_FAULT);
    CHECK(kms_get_u32(page + 40) == KMS_EVENT_ACK_TIMEOUT && kms_get_u32(page + 48) == 21);
    CHECK(kms_crc32(page, 124) == kms_get_u32(page + 124));
    // Manual freeze remains idempotent and rearming starts a fresh window.
    kms_put_u32(command + 4, 2); kms_test_command(command, sizeof(command));
    CHECK(next_record == 605);
    kms_put_u32(command + 4, 1); kms_put_u32(command + 8, 101); kms_test_command(command, sizeof(command));
    CHECK(enabled && !frozen && !next_record && !overwritten && !summary[11]);
    kms_test_record(KMS_T_RX, 1, 0, 0, NULL, 0);
    kms_test_record(KMS_T_FAULT, KMS_EVENT_USB_STALL, 3, 9, NULL, 0);
    CHECK(frozen && next_record == 2);
    kms_test_page(page, sizeof(page)); CHECK(kms_get_u32(page + 8) == 101);
    kms_put_u32(command + 4, 3); kms_test_command(command, sizeof(command));
    kms_test_record(KMS_T_FAULT, KMS_EVENT_ACK_TIMEOUT, 3, 21, NULL, 0);
    CHECK(!enabled && !frozen && !records[0] && !next_record);
    return 0;
}
__declspec(dllexport) int kms_generator_decode_test(const uint8_t *descriptor, unsigned bytes,
                                                  unsigned source, uint32_t sequence, uint32_t seed, unsigned profile)
{
    uint8_t report[KMS_GEN_BYTES + 1] = {1}; kms_sample_t sample;
    kms_generator_pattern(report + 1, source, 99, sequence, seed, profile);
    if (!kms_hid_parse(&parser, descriptor, bytes)) return 1;
    if (!kms_hid_decode(&parser, report, sizeof(report), &sample)) return 2;
    if (sample.state.modifiers != report[1] || memcmp(sample.state.keys, report + 2, 32)) return 3;
    if (sample.state.buttons != ((unsigned)report[34] | (unsigned)report[35] << 8)) return 4;
    int16_t x = (int16_t)((unsigned)report[36] | (unsigned)report[37] << 8);
    int16_t y = (int16_t)((unsigned)report[38] | (unsigned)report[39] << 8);
    if (sample.x != x || sample.y != y || sample.wheel != (int8_t)report[40] || sample.pan != (int8_t)report[41]) return 5;
    if (sample.state.consumer[0] != ((unsigned)report[42] | (unsigned)report[43] << 8)) return 6;
    return 0;
}
__declspec(dllexport) int kms_run_stress_trace_tests(void)
{
    checks = 0;
    uint8_t command[12] = {'K','M','T','1'}, page[KMS_TEST_PAGE], payload[64];
    kms_test_init(122, false);
    CHECK(kms_test_page(page, sizeof(page)) == KMS_TEST_PAGE);
    CHECK((kms_get_u32(page + 20) & KMS_TEST_CAN_RESTART) != 0);
    kms_put_u32(command + 4, KMS_TEST_RESTART);
    kms_put_u32(command + 8, KMS_TEST_RESTART_KEY);
    kms_test_command(command, 8); CHECK(!kms_test_take_restart());
    kms_put_u32(command + 8, 0); kms_test_command(command, sizeof(command));
    CHECK(!kms_test_take_restart());
    kms_put_u32(command + 8, KMS_TEST_RESTART_KEY); kms_test_command(command, sizeof(command));
    CHECK(kms_test_take_restart() && !kms_test_take_restart());
    kms_test_init(123, true);
    kms_test_command(command, sizeof(command)); CHECK(!kms_test_take_restart());
    kms_test_page(page, sizeof(page)); CHECK(!(kms_get_u32(page + 20) & KMS_TEST_CAN_RESTART));
    kms_put_u32(command + 4, 0); kms_put_u32(command + 8, 0);
    memset(payload, 0x5a, sizeof(payload));
    kms_test_mode_init(true); kms_test_init(123, true);
    CHECK(!kms_test_mode_active() && !records[0] && allocations == 0);
    kms_put_u32(command + 4, 1); kms_test_command(command, sizeof(command));
    CHECK(!enabled && allocations == 0); // Ordinary use cannot arm injection/capture.
    fail_mode_write = true; CHECK(!kms_test_mode_set(true) && !kms_test_mode_active());
    fail_mode_write = false; CHECK(kms_test_mode_set(true) && mode_writes == 1);
    CHECK(kms_test_mode_set(true) && mode_writes == 1); // Idempotent, no flash churn.
    kms_test_mode_init(true); CHECK(kms_test_mode_active()); // Power-on retains interrupted session.
    CHECK(kms_test_mode_history_allowed(0xfffffff0u));
    CHECK(!kms_test_mode_history_allowed(10));
    CHECK(kms_test_mode_history_allowed(0xfffffff0u + 60000u));
    fail_mode_write = true; CHECK(!kms_test_mode_set(false) && kms_test_mode_active());
    fail_mode_write = false;
    kms_put_u32(command + 4, 1); kms_put_u32(command + 8, 42); kms_test_command(command, sizeof(command));
    CHECK(enabled && psram_attempts == 4 && internal_attempts == 0);
    CHECK(requested_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (unsigned n = 0; n < 300; ++n) kms_test_record(KMS_T_RAW, n, 2, 3, payload, sizeof(payload));
    kms_put_u32(command + 4, 2); kms_test_command(command, sizeof(command));
    CHECK(kms_test_page(page, sizeof(page)) == KMS_TEST_PAGE);
    CHECK(kms_get_u32(page + 4) == 123 && kms_get_u32(page + 8) == 42);
    CHECK(kms_get_u32(page + 12) == 300 && kms_get_u32(page + 16) == 44);
    CHECK(kms_get_u32(page + 24) == 44 && kms_get_u32(page + 28) == 44);
    CHECK(!memcmp(page + 56, payload, sizeof(payload)));
    CHECK(kms_crc32(page, 124) == kms_get_u32(page + 124));
    kms_test_record(KMS_T_RAW, 999, 0, 0, NULL, 0);
    CHECK(kms_test_page(page, sizeof(page)) == KMS_TEST_PAGE && kms_get_u32(page + 12) == 300);
    kms_put_u32(command + 4, 4); kms_put_u32(command + 8, 255); kms_test_command(command, sizeof(command));
    kms_test_page(page, sizeof(page)); CHECK(kms_get_u32(page + 28) == 299);
    CHECK(kms_test_page(page, 127) == 0);
    kms_put_u32(command + 4, 1); kms_put_u32(command + 8, 43); kms_test_command(command, sizeof(command));
    CHECK(psram_attempts == 4 && internal_attempts == 0); // Re-arm reuses the buffer.
    kms_put_u32(command + 4, 5); kms_put_u32(command + 8, 2); kms_test_command(command, sizeof(command));
    CHECK(!kms_test_drop(KMS_INPUT)); CHECK(kms_test_drop(KMS_INPUT)); CHECK(!kms_test_drop(KMS_ACK));
    CHECK(!kms_test_drop(KMS_INPUT_BATCH)); CHECK(kms_test_drop(KMS_INPUT_BATCH));
    kms_put_u32(command + 4, 6); kms_put_u32(command + 8, 1); kms_test_command(command, sizeof(command));
    CHECK(!kms_test_usb_available());
    kms_put_u32(command + 4, 3); kms_test_command(command, sizeof(command));
    CHECK(!kms_test_drop(KMS_INPUT));
    CHECK(kms_test_usb_available());
    CHECK(!records[0] && frees == 4 && kms_test_mode_active()); // Trace off does not clear restart guard.
    CHECK(mode_writes == 1); // Arms, freezes, injections and off only use RAM.
    CHECK(kms_test_mode_set(false));
    kms_test_mode_init(true); CHECK(!kms_test_mode_active());
    CHECK(kms_test_mode_history_allowed(1) && kms_test_mode_history_allowed(1));
    fail_mode_read = true; kms_test_mode_init(true); CHECK(kms_test_mode_active());
    fail_mode_write = true; CHECK(!kms_test_mode_set(true)); // Fail-closed is not an acknowledged durable session.
    fail_mode_write = false;
    fail_mode_read = false;
    CHECK(kms_test_mode_set(false));
    kms_test_mode_init(false); kms_test_init(456, false);
    fail_alloc = true; kms_put_u32(command + 4, 1); kms_test_command(command, sizeof(command));
    CHECK(!enabled && allocation_failed && kms_test_mode_active());
    kms_test_page(page, sizeof(page)); CHECK(kms_get_u32(page + 20) & 4);
    fail_alloc = false; kms_test_command(command, sizeof(command));
    CHECK(enabled && records[0] && !allocation_failed);
    CHECK(psram_attempts == 4); // Receivers keep using their internal RAM.
    kms_put_u32(command + 4, 3); kms_test_command(command, sizeof(command));
    CHECK(!records[0] && !kms_test_mode_active() && kms_test_usb_available());
    kms_test_mode_init(true); kms_test_init(789, true);
    CHECK(kms_test_console("test-mode on") && pauses == 1 && kms_test_mode_active());
    unsigned writes = mode_writes;
    CHECK(kms_test_console("test-mode") && mode_writes == writes && pauses == 1);
    CHECK(kms_test_console("test-mode off") && pauses == 2 && !kms_test_mode_active());
    CHECK(kms_test_mode_set(true)); // Python oracle tests explicitly arm master captures.
    unsigned before_internal = internal_attempts;
    fail_psram = true; kms_put_u32(command + 4, 1); kms_test_command(command, sizeof(command));
    CHECK(enabled && records[0] && internal_attempts == before_internal + 4);
    kms_put_u32(command + 4, 3); kms_test_command(command, sizeof(command));
    fail_alloc = true; kms_put_u32(command + 4, 1); kms_test_command(command, sizeof(command));
    CHECK(!enabled && !records[0] && allocation_failed && kms_test_mode_active());
    fail_alloc = fail_psram = false; kms_test_command(command, sizeof(command));
    CHECK(enabled && records[0] && !allocation_failed);
    kms_test_record(KMS_T_RADIO_SEND_CALL, 137, 3210, 0, NULL, 0);
    kms_test_record(KMS_T_LINK_HANDLED, 43, KMS_ACK, 5, NULL, 0);
    kms_put_u32(command + 4, 2); kms_test_command(command, sizeof(command));
    kms_test_page(page, sizeof(page));
    CHECK(kms_get_u32(page + 12) == 2 && kms_get_u32(page + 36) == KMS_T_RADIO_SEND_CALL);
    CHECK(kms_get_u32(page + 40) == 137 && kms_get_u32(page + 44) == 3210);
    kms_put_u32(command + 4, 4); kms_put_u32(command + 8, 1); kms_test_command(command, sizeof(command));
    kms_test_page(page, sizeof(page));
    CHECK(kms_get_u32(page + 36) == KMS_T_LINK_HANDLED && kms_get_u32(page + 40) == 43);
    CHECK(kms_get_u32(page + 44) == KMS_ACK && kms_get_u32(page + 48) == 5);
    CHECK(summary[11] == 0 && summary[12] == 0); // Timing records are not fault/injection events.
    kms_put_u32(command + 4, 3); kms_test_command(command, sizeof(command));
    int failure = first_fault_retention(true); if (failure) return failure;
    failure = first_fault_retention(false); if (failure) return failure;
    kms_test_mode_init(false); kms_test_init(456, false);
    unsigned before_frees = frees;
    allocation_limit = 2; // Total free RAM is insufficient: release partial ownership.
    kms_put_u32(command + 4, 1); kms_put_u32(command + 8, 91); kms_test_command(command, sizeof(command));
    CHECK(!enabled && !records[0] && allocation_failed && frees == before_frees + 2);
    for (unsigned i = 0; i < 4; ++i) CHECK(!trace_used[i] && !records[i]);
    allocation_limit = 4; // Four separated 5,888-byte blocks; no 23 KB block exists.
    kms_test_command(command, sizeof(command));
    CHECK(enabled && !allocation_failed);
    for (unsigned i = 0; i < 4; ++i) CHECK(trace_used[i] && records[i] == trace_memory[i]);
    for (unsigned n = 0; n < 300; ++n) kms_test_record(KMS_T_RAW, n, 0, 0, NULL, 0);
    kms_put_u32(command + 4, 2); kms_test_command(command, sizeof(command));
    for (unsigned n = 0; n < 256; ++n) {
        kms_put_u32(command + 4, 4); kms_put_u32(command + 8, n); kms_test_command(command, sizeof(command));
        kms_test_page(page, sizeof(page));
        CHECK(kms_get_u32(page + 12) == 300 && kms_get_u32(page + 16) == 44);
        CHECK(kms_get_u32(page + 28) == n + 44 && kms_get_u32(page + 40) == n + 44);
        CHECK(kms_crc32(page, 124) == kms_get_u32(page + 124));
    }
    before_frees = frees;
    kms_put_u32(command + 4, 3); kms_test_command(command, sizeof(command));
    CHECK(!enabled && frees == before_frees + 4);
    kms_test_command(command, sizeof(command));
    CHECK(frees == before_frees + 4); // Repeated off never frees a chunk twice.
    for (unsigned i = 0; i < 4; ++i) CHECK(!trace_used[i] && !records[i]);
    return 0;
}
__declspec(dllexport) unsigned kms_stress_trace_test_count(void) { return checks; }
