#pragma once
#include "kms_core.h"

typedef struct { kms_single_packet_t packet; bool pending; } kms_ack_t;
typedef bool (*kms_ack_send_fn)(void *, const kms_packet_t *);
void kms_ack_prepare(kms_ack_t *, const kms_packet_t *accepted);
void kms_ack_flush(kms_ack_t *, kms_ack_send_fn, void *);
