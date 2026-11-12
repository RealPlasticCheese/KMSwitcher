#include "kms_ack.h"

void kms_ack_prepare(kms_ack_t *ack, const kms_packet_t *accepted)
{
    ack->packet = accepted->single; ack->packet.type = KMS_ACK; ack->pending = true;
}
void kms_ack_flush(kms_ack_t *ack, kms_ack_send_fn send, void *ctx)
{
    if (!ack->pending) return;
    // Submission failure includes a busy radio. Retain the receipt until the
    // next tick; never require the master to retransmit already accepted input.
    kms_packet_t packet = {.single = ack->packet};
    if (send(ctx, &packet)) ack->pending = false;
}
