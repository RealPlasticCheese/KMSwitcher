#include "pair_crypto.h"
#include <string.h>

bool kms_exchange_start(kms_exchange_t *x)
{
    kms_exchange_end(x);
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&a, KMS_PSA_KEY_BITS);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);
    size_t n = 0;
    bool ok = psa_generate_key(&a, &x->key) == PSA_SUCCESS &&
              psa_export_public_key(x->key, x->public_key, sizeof(x->public_key), &n) == PSA_SUCCESS &&
              n == sizeof(x->public_key);
    psa_reset_key_attributes(&a);
    if (!ok) kms_exchange_end(x);
    return ok;
}
void kms_exchange_end(kms_exchange_t *x)
{
    if (x->key) psa_destroy_key(x->key);
    memset(x, 0, sizeof(*x));
}
bool kms_exchange_key(kms_exchange_t *x, const uint8_t remote[KMS_PUBLIC_BYTES],
                      const uint8_t nonce[KMS_NONCE_BYTES], const uint8_t master[6],
                      const uint8_t receiver[6], unsigned slot, bool is_master,
                      uint8_t key[KMS_LINK_KEY_BYTES])
{
    uint8_t shared[KMS_PUBLIC_BYTES], digest[KMS_PUBLIC_BYTES];
    size_t n = 0;
    if (psa_raw_key_agreement(PSA_ALG_ECDH, x->key, remote, KMS_PUBLIC_BYTES,
                              shared, sizeof(shared), &n) != PSA_SUCCESS || n != sizeof(shared)) return false;
    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    uint8_t s = (uint8_t)slot;
    static const uint8_t domain[] = "KMSwitcher pairing v1";
    const uint8_t *mp = is_master ? x->public_key : remote;
    const uint8_t *rp = is_master ? remote : x->public_key;
    bool ok = psa_hash_setup(&h, PSA_ALG_SHA_256) == PSA_SUCCESS &&
              psa_hash_update(&h, domain, sizeof(domain) - 1) == PSA_SUCCESS &&
              psa_hash_update(&h, shared, sizeof(shared)) == PSA_SUCCESS &&
              psa_hash_update(&h, nonce, KMS_NONCE_BYTES) == PSA_SUCCESS &&
              psa_hash_update(&h, master, 6) == PSA_SUCCESS &&
              psa_hash_update(&h, receiver, 6) == PSA_SUCCESS &&
              psa_hash_update(&h, mp, KMS_PUBLIC_BYTES) == PSA_SUCCESS &&
              psa_hash_update(&h, rp, KMS_PUBLIC_BYTES) == PSA_SUCCESS &&
              psa_hash_update(&h, &s, sizeof(s)) == PSA_SUCCESS &&
              psa_hash_finish(&h, digest, sizeof(digest), &n) == PSA_SUCCESS;
    psa_hash_abort(&h);
    if (ok) memcpy(key, digest, KMS_LINK_KEY_BYTES);
    memset(shared, 0, sizeof(shared)); memset(digest, 0, sizeof(digest));
    return ok;
}
bool kms_pair_proof(const uint8_t key[KMS_LINK_KEY_BYTES], const uint8_t nonce[KMS_NONCE_BYTES],
                    unsigned slot, bool done, uint8_t proof[KMS_NONCE_BYTES])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    uint8_t message[KMS_NONCE_BYTES + 2], tag[KMS_PUBLIC_BYTES]; size_t n = 0;
    memcpy(message, nonce, KMS_NONCE_BYTES);
    message[KMS_NONCE_BYTES] = (uint8_t)slot;
    message[KMS_NONCE_BYTES + 1] = done ? KMS_PAIR_DONE : KMS_PAIR_CONFIRM;
    bool ok = psa_import_key(&a, key, KMS_LINK_KEY_BYTES, &id) == PSA_SUCCESS &&
              psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, sizeof(message),
                               tag, sizeof(tag), &n) == PSA_SUCCESS;
    if (id) psa_destroy_key(id);
    psa_reset_key_attributes(&a);
    if (ok) memcpy(proof, tag, KMS_NONCE_BYTES);
    memset(tag, 0, sizeof(tag));
    return ok;
}
