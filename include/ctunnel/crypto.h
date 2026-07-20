#ifndef CTUNNEL_CRYPTO_H
#define CTUNNEL_CRYPTO_H

#include "ctunnel.h"
#include <stddef.h>
#include <stdint.h>

#define CT_CRYPTO_SIGN_PUBLIC_KEY_SIZE 32
#define CT_CRYPTO_SIGN_PRIVATE_KEY_SIZE 64
#define CT_CRYPTO_SIGNATURE_SIZE 64
#define CT_CRYPTO_KX_PUBLIC_KEY_SIZE 32
#define CT_CRYPTO_KX_PRIVATE_KEY_SIZE 32
#define CT_CRYPTO_SHARED_SECRET_SIZE 32
#define CT_CRYPTO_AEAD_KEY_SIZE 32
#define CT_CRYPTO_AEAD_NONCE_SIZE 24
#define CT_CRYPTO_AEAD_TAG_SIZE 16
#define CT_CRYPTO_NONCE_BASE_SIZE 16

#define CT_ED_PUBLIC CT_CRYPTO_SIGN_PUBLIC_KEY_SIZE
#define CT_ED_SECRET CT_CRYPTO_SIGN_PRIVATE_KEY_SIZE
#define CT_X_PUBLIC CT_CRYPTO_KX_PUBLIC_KEY_SIZE
#define CT_X_SECRET CT_CRYPTO_KX_PRIVATE_KEY_SIZE
#define CT_KEY CT_CRYPTO_AEAD_KEY_SIZE
#define CT_NONCE_BASE CT_CRYPTO_NONCE_BASE_SIZE
#define CT_AEAD_TAG CT_CRYPTO_AEAD_TAG_SIZE

typedef struct {
    uint8_t control_c2s[CT_CRYPTO_AEAD_KEY_SIZE];
    uint8_t control_s2c[CT_CRYPTO_AEAD_KEY_SIZE];
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    uint8_t data_master[CT_CRYPTO_AEAD_KEY_SIZE];
#endif
    uint8_t work_auth_key[CT_CRYPTO_AEAD_KEY_SIZE];
    uint8_t stream_auth_key[CT_CRYPTO_AEAD_KEY_SIZE];
    uint8_t nonce_c2s[CT_CRYPTO_NONCE_BASE_SIZE];
    uint8_t nonce_s2c[CT_CRYPTO_NONCE_BASE_SIZE];
    uint64_t session_id;
} ct_session_keys;

int ct_crypto_init(void);
int ct_crypto_random(uint8_t *output, size_t output_length);
int ct_crypto_sign_keypair(uint8_t public_key[32], uint8_t private_key[64]);
int ct_crypto_sign(uint8_t signature[64], const uint8_t private_key[64], const uint8_t *message,
                   size_t message_length);
int ct_crypto_verify(const uint8_t signature[64], const uint8_t public_key[32],
                     const uint8_t *message, size_t message_length);
int ct_crypto_kx_keypair(uint8_t public_key[32], uint8_t private_key[32]);
int ct_crypto_kx(uint8_t shared_secret[32], const uint8_t private_key[32],
                 const uint8_t peer_public_key[32]);
int ct_crypto_kdf(uint8_t *output, size_t output_length, const uint8_t *key_material,
                  size_t key_material_length, const uint8_t *salt, size_t salt_length,
                  const uint8_t *context, size_t context_length);
int ct_crypto_aead_encrypt(uint8_t *ciphertext, uint8_t tag[16], const uint8_t *plaintext,
                           size_t plaintext_length, const uint8_t *associated_data,
                           size_t associated_data_length, const uint8_t nonce[24],
                           const uint8_t key[32]);
int ct_crypto_aead_decrypt(uint8_t *plaintext, const uint8_t *ciphertext, size_t ciphertext_length,
                           const uint8_t tag[16], const uint8_t *associated_data,
                           size_t associated_data_length, const uint8_t nonce[24],
                           const uint8_t key[32]);
void ct_crypto_hash(uint8_t output[32], const uint8_t *message, size_t message_length);
void ct_crypto_mac(uint8_t output[32], const uint8_t key[32], const uint8_t *message,
                   size_t message_length);
void ct_crypto_wipe(void *memory, size_t length);
int ct_crypto_equal(const uint8_t *left, const uint8_t *right, size_t length);
const char *ct_crypto_backend_name(void);
const char *ct_crypto_backend_version(void);

int ct_load_public_key(const char *, uint8_t[CT_ED_PUBLIC]);
int ct_load_private_key(const char *, uint8_t[CT_ED_SECRET]);
#ifdef CONFIG_FEATURE_KEYGEN
int ct_keygen_files(const char *, const char *, char *, size_t);
#endif
#ifdef CONFIG_FEATURE_FINGERPRINT
int ct_fingerprint_file(const char *, char *, size_t);
#endif
int ct_ed_sign(uint8_t[64], const uint8_t *, size_t, const uint8_t[64]);
int ct_ed_verify(const uint8_t[64], const uint8_t *, size_t, const uint8_t[32]);
int ct_x_keypair(uint8_t[32], uint8_t[32]);
int ct_x_shared(uint8_t[32], const uint8_t[32], const uint8_t[32]);
int ct_derive_session(const uint8_t[32], const uint8_t *, size_t, ct_session_keys *);
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
int ct_derive_data(const uint8_t[32], uint64_t, const uint8_t[32], const uint8_t[32], const char *,
                   ct_enc_mode, int, uint8_t[32], uint8_t[CT_NONCE_BASE]);
#endif
int ct_aead_encrypt(ct_cipher, const uint8_t[32], const uint8_t[CT_NONCE_BASE], uint64_t,
                    const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *, size_t *);
int ct_aead_decrypt(ct_cipher, const uint8_t[32], const uint8_t[CT_NONCE_BASE], uint64_t,
                    const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *, size_t *);
int ct_cipher_available(ct_cipher);
#ifdef CONFIG_FEATURE_TEST_HOOKS
void ct_test_nonce_tracker_reset(void);
#endif

#endif
