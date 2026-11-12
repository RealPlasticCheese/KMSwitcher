#pragma once
#include "kms_stream.h"

enum { KMS_STREAM_REPORT = 36 };
void kms_stream_runtime_init(void);
bool kms_stream_enqueue(const kms_management_frame_t *);
bool kms_stream_enqueue_profile(const kms_management_frame_t *, unsigned);
unsigned kms_stream_pending(void);
unsigned kms_stream_capacity(void);
unsigned kms_stream_select(bool bulk, unsigned *indices, kms_stream_batch_t *, unsigned maximum);
void kms_stream_commit(const unsigned *indices, unsigned count);
void kms_stream_drop_clipboard(void);
unsigned kms_stream_requested_profile(void);
bool kms_stream_set_profile(unsigned);
size_t kms_stream_usb_get(uint8_t *, size_t);
void kms_stream_usb_set(const uint8_t *, size_t);
