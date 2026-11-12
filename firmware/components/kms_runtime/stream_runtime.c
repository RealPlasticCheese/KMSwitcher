#include "kms_stream_runtime.h"
#include "kms_memory.h"
#include "sdkconfig.h"
#include <string.h>

static kms_stream_pool_t pool;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned requested;

void kms_stream_runtime_init(void)
{
    // Four bulk cells plus two reserved control cells on the RAM-constrained S2.
    // The S3's eight cells prefer PSRAM; fallback never increases this budget.
#if CONFIG_IDF_TARGET_ESP32S3
    unsigned count = KMS_STREAM_SLOTS;
#else
    unsigned count = KMS_STREAM_WINDOW + 2;
#endif
    kms_stream_slot_t *slots = kms_memory_calloc(count, sizeof(*slots));
    kms_stream_pool_init(&pool, slots, slots ? count : 0);
}
bool kms_stream_enqueue(const kms_management_frame_t *frame)
{ return kms_stream_enqueue_profile(frame, kms_stream_requested_profile()); }
bool kms_stream_enqueue_profile(const kms_management_frame_t *frame, unsigned profile)
{
    portENTER_CRITICAL(&lock);
    bool accepted = (!kms_stream_bulk(frame) || kms_stream_free(&pool) > 2) &&
        kms_stream_submit(&pool, frame, profile);
    portEXIT_CRITICAL(&lock); return accepted;
}
unsigned kms_stream_capacity(void)
{
    portENTER_CRITICAL(&lock); unsigned free = kms_stream_free(&pool); portEXIT_CRITICAL(&lock);
    unsigned capacity = free > 2 ? free - 2 : 0;
    return capacity > KMS_STREAM_WINDOW ? KMS_STREAM_WINDOW : capacity;
}
unsigned kms_stream_pending(void)
{
    portENTER_CRITICAL(&lock); unsigned n = pool.capacity - kms_stream_free(&pool);
    portEXIT_CRITICAL(&lock); return n;
}
unsigned kms_stream_select(bool bulk, unsigned *indices, kms_stream_batch_t *batch, unsigned maximum)
{
    portENTER_CRITICAL(&lock);
    unsigned n = kms_stream_peek(&pool, bulk, indices, maximum);
    batch->count = (uint8_t)n;
    for (unsigned i = 0; i < n; ++i) batch->frames[i] = pool.slots[indices[i]].frame;
    if (n) {
        batch->profile = pool.slots[indices[0]].profile;
        batch->sender = batch->frames[0].sender; batch->target = batch->frames[0].target;
        batch->slot = batch->frames[0].slot;
    }
    portEXIT_CRITICAL(&lock); return n;
}
void kms_stream_commit(const unsigned *indices, unsigned count)
{ portENTER_CRITICAL(&lock); kms_stream_release(&pool, indices, count); portEXIT_CRITICAL(&lock); }
void kms_stream_drop_clipboard(void)
{
    portENTER_CRITICAL(&lock);
    for (unsigned i = 0; i < pool.capacity; ++i)
        if (pool.slots[i].frame.op == KMS_M_CLIPBOARD) pool.slots[i].used = false;
    portEXIT_CRITICAL(&lock);
}
unsigned kms_stream_requested_profile(void)
{ portENTER_CRITICAL(&lock); unsigned p = requested; portEXIT_CRITICAL(&lock); return p; }
bool kms_stream_set_profile(unsigned profile)
{
    if (profile >= KMS_STREAM_PROFILE_COUNT) return false;
    portENTER_CRITICAL(&lock); requested = profile; portEXIT_CRITICAL(&lock); return true;
}
size_t kms_stream_usb_get(uint8_t *out, size_t capacity)
{
    if (capacity < 32) return 0;
    memset(out, 0, 32); memcpy(out, "KMS2", 4); out[4] = KMS_STREAM_VERSION;
    portENTER_CRITICAL(&lock);
    unsigned free = kms_stream_free(&pool);
    out[5] = (uint8_t)(free > 2 ? free - 2 : 0); out[6] = KMS_STREAM_WINDOW;
    out[7] = (uint8_t)requested; out[8] = (uint8_t)pool.capacity; out[9] = (uint8_t)free;
    portEXIT_CRITICAL(&lock);
    kms_put_u32(out + 12, kms_crc32(out, 12)); return 32;
}
void kms_stream_usb_set(const uint8_t *data, size_t length)
{
    if (!data || (length != 32 && length != 192) || memcmp(data, "KMS2", 4) ||
        data[4] != KMS_STREAM_VERSION || kms_get_u32(data + 12) != kms_crc32(data, 12)) return;
    for (size_t i = 16; i < length; ++i) if (data[i]) return;
    (void)kms_stream_set_profile(data[7]);
}
