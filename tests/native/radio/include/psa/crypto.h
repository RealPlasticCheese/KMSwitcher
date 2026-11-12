#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int psa_key_id_t;
typedef int psa_key_attributes_t;
#define PSA_KEY_ATTRIBUTES_INIT 0
enum { PSA_SUCCESS, PSA_KEY_TYPE_HMAC, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_SHA_256 };
#define PSA_ALG_HMAC(a) (a)
void psa_set_key_type(psa_key_attributes_t *, int);
void psa_set_key_usage_flags(psa_key_attributes_t *, int);
void psa_set_key_algorithm(psa_key_attributes_t *, int);
int psa_import_key(const psa_key_attributes_t *, const uint8_t *, size_t, psa_key_id_t *);
int psa_mac_compute(psa_key_id_t, int, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
void psa_destroy_key(psa_key_id_t);
void psa_reset_key_attributes(psa_key_attributes_t *);
