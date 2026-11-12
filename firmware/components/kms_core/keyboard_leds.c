#include "kms_core.h"

void kms_led_state_set(kms_led_state_t *s, bool valid, uint8_t leds)
{
    leds = valid ? leds & KMS_LOCK_MASK : 0;
    if (s->valid == valid && s->leds == leds) return;
    s->valid = valid; s->leds = leds; ++s->sequence;
}

bool kms_led_feedback_accept(kms_led_feedback_t *f, const kms_packet_t *p,
                              uint32_t master_boot, uint32_t receiver_boot,
                              unsigned slot, uint32_t generation)
{
    if (!slot || p->slot != slot || p->generation != generation ||
        (p->flags & (KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE)) != (KMS_FLAG_USB_READY | KMS_FLAG_ACTIVE)) return false;
    if (p->type == KMS_STATUS) {
        if (p->session != receiver_boot || p->receiver != master_boot) return false;
    } else if (p->type == KMS_ACK) {
        if (p->session != master_boot || p->receiver != receiver_boot) return false;
    } else return false;
    if (f->received && !kms_newer(p->keyboard_leds.sequence, f->state.sequence)) return false;
    f->state = p->keyboard_leds;
    f->state.leds = f->state.valid ? f->state.leds & KMS_LOCK_MASK : 0;
    f->received = true;
    return true;
}
