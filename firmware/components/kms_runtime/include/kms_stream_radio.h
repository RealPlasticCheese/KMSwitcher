#pragma once
#include "kms_stream_runtime.h"

typedef bool (*kms_stream_send_fn)(const uint8_t *, const uint8_t *, size_t);
typedef bool (*kms_stream_peer_fn)(unsigned, uint8_t *, uint32_t *);
typedef unsigned (*kms_stream_space_fn)(void);
typedef void (*kms_stream_receive_fn)(const kms_management_frame_t *, const uint8_t *, int8_t);
void kms_stream_radio_init(uint32_t, kms_stream_space_fn, kms_stream_receive_fn);
void kms_stream_radio_receive(const uint8_t *, size_t, const uint8_t *, int8_t);
bool kms_stream_radio_control(const kms_management_frame_t *);
void kms_stream_radio_tick(bool, uint32_t, kms_stream_send_fn, kms_stream_peer_fn);
void kms_stream_radio_rate(const uint8_t *, const uint8_t *, size_t);
void kms_stream_radio_complete(bool success, uint32_t elapsed);
size_t kms_stream_radio_status(uint8_t *, size_t);
uint32_t kms_stream_local_status(unsigned slot, unsigned incoming);
unsigned kms_stream_radio_source_profile(unsigned slot);
