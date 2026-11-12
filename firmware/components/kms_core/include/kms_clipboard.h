#pragma once
#include "kms_desktop.h"

// Feature reports span EP0 packets; keyboard/mouse endpoints stay unchanged.
enum { KMS_C_BYTES = 192, KMS_C_DATA = 160, KMS_C_SEND_REPORT = 34, KMS_C_RECEIVE_REPORT = 35,
       KMS_C_QUERY = 1, KMS_C_INFO, KMS_C_BEGIN, KMS_C_DATA_CHUNK, KMS_C_END, KMS_C_ACK,
       KMS_C_CANCEL, KMS_C_ERROR };
typedef struct {
    uint8_t kind, source, target, length;
    uint32_t source_session, target_session, sequence, transfer, offset;
    uint8_t data[KMS_C_DATA];
} kms_clipboard_packet_t;
bool kms_clipboard_encode(const kms_clipboard_packet_t *, uint8_t *);
bool kms_clipboard_decode(kms_clipboard_packet_t *, const uint8_t *, size_t);
bool kms_clipboard_authorized(const kms_desktop_master_t *, const kms_clipboard_packet_t *,
                              unsigned source, uint32_t boot, uint32_t now);
