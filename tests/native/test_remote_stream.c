#include "kms_stream_runtime.h"
#include "kms_core.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
static uint32_t boot_id = 42, clock_ms = 100;
static kms_remote_upload_t remote_uploads[7];
static unsigned printed;
static int state_lock;
static uint32_t kms_now(void) { return clock_ms; }
static void capture(const char *format, unsigned slot, const char *hex)
{ (void)format; (void)slot; (void)hex; ++printed; }
#define printf capture
static bool send_frame(const kms_management_frame_t *frame) { return kms_stream_enqueue(frame); }
#include "remote_stream.inc"
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_run_remote_stream_tests(void)
{
    kms_stream_runtime_init();
    kms_management_frame_t q = {.op = KMS_M_OTA_WRITE, .slot = 1, .target = 99, .length = 192};
    kms_put_u32(q.data, 123);
    for (unsigned i = 0; i < 4; ++i) { q.id = i + 1; q.offset = i * 188; stream_forward(&q); }
    CHECK(kms_stream_pending() == 4 && !printed);
    q.id = 5; q.offset = 752; stream_forward(&q);
    CHECK(kms_stream_pending() == 4 && printed == 1); // BUSY, with the earlier requests intact.
    kms_management_frame_t reply = {.op = KMS_M_OTA_WRITE, .slot = 1, .sender = 99,
        .target = boot_id, .id = 2, .offset = 188, .flags = KMS_M_REPLY, .length = 4};
    kms_put_u32(reply.data, 376);
    reply.target = 43; CHECK(!stream_reply(&reply)); reply.target = boot_id;
    reply.offset = 0; CHECK(!stream_reply(&reply)); reply.offset = 188;
    reply.sender = 100; CHECK(!stream_reply(&reply)); reply.sender = 99;
    CHECK(stream_reply(&reply) && printed == 2);
    CHECK(!stream_reply(&reply)); // Duplicate cannot release a different pending request.
    q.id = 3; q.offset = 376; stream_forward(&q); CHECK(printed == 3); // Pool pressure remains bounded.
    unsigned indices[4]; kms_stream_batch_t batch = {0};
    unsigned count = kms_stream_select(true, indices, &batch, 4); kms_stream_commit(indices, count);
    stream_forward(&q); CHECK(printed == 3 && kms_stream_pending() == 1);
    unsigned matching = 0;
    for (unsigned i = 0; i < 4; ++i) matching += stream_requests[i].id == 3;
    CHECK(matching == 1); // A retry keeps the same pending identity.
    reply.id = 3; reply.offset = 376; reply.sender = 100; reply.status = KMS_M_WRONG_BOOT;
    CHECK(stream_reply(&reply) && printed == 4);
    stream_reset(1);
    for (unsigned i = 0; i < 4; ++i) CHECK(!stream_requests[i].id);
    q.id = 6; stream_forward(&q);
    CHECK(printed == 4 && kms_stream_pending() == 2);
    return 0;
}
