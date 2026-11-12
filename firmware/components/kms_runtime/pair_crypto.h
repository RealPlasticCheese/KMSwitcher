#pragma once
#include "kms_core.h"
#include "psa/crypto.h"
enum { KMS_PUBLIC_BYTES = 32, KMS_NONCE_BYTES = 16, KMS_LINK_KEY_BYTES = 16, KMS_PSA_KEY_BITS = 255 };
typedef struct {
    psa_key_id_t key;
    uint8_t public_key[KMS_PUBLIC_BYTES];
} kms_exchange_t;
bool kms_exchange_start(kms_exchange_t *);
void kms_exchange_end(kms_exchange_t *);
bool kms_exchange_key(kms_exchange_t *, const uint8_t remote[KMS_PUBLIC_BYTES],
                      const uint8_t nonce[KMS_NONCE_BYTES], const uint8_t master[6],
                      const uint8_t receiver[6], unsigned slot, bool is_master,
                      uint8_t key[KMS_LINK_KEY_BYTES]);
bool kms_pair_proof(const uint8_t key[KMS_LINK_KEY_BYTES], const uint8_t nonce[KMS_NONCE_BYTES],
                    unsigned slot, bool done, uint8_t proof[KMS_NONCE_BYTES]);
