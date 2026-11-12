#define KMS_NVS_NAMESPACE "kmswitcher"
#include "../../firmware/components/kms_runtime/jiggler_store.c"
static unsigned checks, writes;
static uint32_t clock_ms = 100;
static uint8_t stored[16];
static bool present, failure;
uint32_t kms_now(void) { return clock_ms; }
void kms_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h)
{ (void)name; (void)mode; *h = 1; return failure ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    (void)h; (void)key; if (!present) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < sizeof(stored)) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, stored, sizeof(stored)); *size = sizeof(stored); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    (void)h; (void)key; if (failure || size != sizeof(stored)) return ESP_FAIL;
    memcpy(stored, data, size); present = true; ++writes; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return failure ? ESP_FAIL : ESP_OK; }
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_run_jiggler_store_tests(void)
{
    checks = writes = 0; kms_jiggler_t j;
    kms_jiggler_store_load(&j, clock_ms);
    CHECK(!j.enabled && j.interval_seconds == 30 && j.distance == 1 && !j.session && !j.sequence);
    kms_jiggler_store_maintain(); CHECK(!writes);
    kms_packet_t q = {.type = KMS_JIGGLER, .session = 99, .sequence = 1, .receiver = 22,
        .flags = KMS_FLAG_JIGGLER_ENABLED | (KMS_JIGGLER_RANDOM << KMS_JIGGLER_MODE_SHIFT), .motion = {5, 0, 0, 3}};
    CHECK(kms_jiggler_configure(&j, &q, clock_ms)); kms_jiggler_store_update(&j); CHECK(!writes);
    kms_jiggler_store_maintain(); CHECK(writes == 1 && !dirty);
    for (unsigned i = 0; i < 20; ++i) { kms_jiggler_store_update(&j); clock_ms += 1000; kms_jiggler_store_maintain(); }
    CHECK(writes == 1);
    kms_jiggler_store_load(&j, 1234);
    CHECK(j.enabled && j.interval_seconds == 5 && j.distance == 3 && j.mode == KMS_JIGGLER_RANDOM);
    CHECK(!j.session && !j.sequence && !j.running && j.changed == 1234);
    kms_jiggler_motion_t motion; CHECK(!kms_jiggler_motion(&j, 6233, &motion) && kms_jiggler_motion(&j, 6234, &motion));
    kms_jiggler_sync_t sync = {.known = true, .boot = 21};
    kms_packet_t status = {.type = KMS_STATUS, .session = 22, .flags = KMS_FLAG_JIGGLER_SUPPORTED | KMS_FLAG_JIGGLER_ENABLED |
        KMS_FLAG_JIGGLER_MODES | KMS_FLAG_JIGGLER_DISTANCE | (KMS_JIGGLER_RANDOM << KMS_JIGGLER_MODE_SHIFT), .motion = {5, 0, 0, 3}};
    kms_jiggler_sync_status(&sync, &status, 99, 5, 3); CHECK(sync.enabled && sync.mode == KMS_JIGGLER_RANDOM && !sync.pending);
    j.enabled = false; kms_jiggler_store_update(&j); failure = true;
    CHECK(!kms_jiggler_store_flush() && dirty);
    unsigned previous = writes; kms_jiggler_store_maintain(); CHECK(writes == previous);
    failure = false; clock_ms += 1000; kms_jiggler_store_maintain(); CHECK(!dirty && writes == previous + 1);
    kms_jiggler_store_load(&j, 2000); CHECK(!j.enabled && j.mode == KMS_JIGGLER_RANDOM && j.distance == 3);
    for (unsigned i = 0; i < sizeof(stored); ++i) {
        stored[i] ^= 1; kms_jiggler_store_load(&j, 3000);
        CHECK(!j.enabled && j.interval_seconds == 30 && j.distance == 1); stored[i] ^= 1;
    }
    j.enabled = true; kms_jiggler_store_update(&j); CHECK(kms_jiggler_store_flush());
    kms_jiggler_store_load(&j, 4000); CHECK(j.enabled);
    return 0;
}
__declspec(dllexport) unsigned kms_jiggler_store_test_count(void) { return checks; }
