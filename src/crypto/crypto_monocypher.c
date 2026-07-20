#include "ctunnel/crypto.h"
#include "platform/platform.h"
#include "protocol/protocol.h"
#include <errno.h>
#include <monocypher.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#define PUB_PREFIX "CTUNNEL-EDDSA-BLAKE2B-PUBLIC-v1:"
#define SEC_PREFIX "CTUNNEL-EDDSA-BLAKE2B-PRIVATE-v1:"

#ifdef CONFIG_FEATURE_KEYGEN
static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(char *out, size_t cap, const uint8_t *in, size_t length) {
    size_t needed = ((length + 2) / 3) * 4 + 1;
    if (cap < needed)
        return -1;
    size_t i = 0, o = 0;
    while (i < length) {
        size_t left = length - i;
        uint32_t value = (uint32_t)in[i] << 16;
        if (left > 1)
            value |= (uint32_t)in[i + 1] << 8;
        if (left > 2)
            value |= in[i + 2];
        out[o++] = b64_alphabet[(value >> 18) & 63];
        out[o++] = b64_alphabet[(value >> 12) & 63];
        out[o++] = left > 1 ? b64_alphabet[(value >> 6) & 63] : '=';
        out[o++] = left > 2 ? b64_alphabet[value & 63] : '=';
        i += left >= 3 ? 3 : left;
    }
    out[o] = 0;
    return 0;
}
#endif

static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static int b64_decode(uint8_t *out, size_t expected, const char *in, size_t length) {
    if (length == 0 || (length & 3) != 0)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < length; i += 4) {
        int a = b64_value((unsigned char)in[i]);
        int b = b64_value((unsigned char)in[i + 1]);
        int c = in[i + 2] == '=' ? -2 : b64_value((unsigned char)in[i + 2]);
        int d = in[i + 3] == '=' ? -2 : b64_value((unsigned char)in[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2) ||
            ((c == -2 || d == -2) && i + 4 != length) || (c == -2 && (b & 15)) ||
            (d == -2 && c >= 0 && (c & 3)))
            return -1;
        uint32_t value = (uint32_t)a << 18 | (uint32_t)b << 12;
        if (c >= 0)
            value |= (uint32_t)c << 6;
        if (d >= 0)
            value |= (uint32_t)d;
        if (o >= expected)
            return -1;
        out[o++] = (uint8_t)(value >> 16);
        if (c >= 0) {
            if (o >= expected)
                return -1;
            out[o++] = (uint8_t)(value >> 8);
        }
        if (d >= 0) {
            if (o >= expected)
                return -1;
            out[o++] = (uint8_t)value;
        }
    }
    return o == expected ? 0 : -1;
}

#if defined(CONFIG_FEATURE_KEYGEN) || defined(CONFIG_FEATURE_FINGERPRINT)
static void hex_encode(char *out, const uint8_t *in, size_t length) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 15];
    }
    out[length * 2] = 0;
}
#endif

int ct_crypto_init(void) {
    return 0;
}

int ct_crypto_random(uint8_t *output, size_t output_length) {
    int result = ct_platform_random(output, output_length);
    if (result)
        crypto_wipe(output, output_length);
    return result;
}

int ct_crypto_sign_keypair(uint8_t public_key[32], uint8_t private_key[64]) {
    uint8_t seed[32];
    if (ct_crypto_random(seed, sizeof seed))
        return -1;
    crypto_eddsa_key_pair(private_key, public_key, seed);
    crypto_wipe(seed, sizeof seed);
    return 0;
}

int ct_crypto_sign(uint8_t signature[64], const uint8_t private_key[64], const uint8_t *message,
                   size_t message_length) {
    crypto_eddsa_sign(signature, private_key, message, message_length);
    return 0;
}

int ct_crypto_verify(const uint8_t signature[64], const uint8_t public_key[32],
                     const uint8_t *message, size_t message_length) {
    return crypto_eddsa_check(signature, public_key, message, message_length);
}

int ct_crypto_kx_keypair(uint8_t public_key[32], uint8_t private_key[32]) {
    if (ct_crypto_random(private_key, 32))
        return -1;
    crypto_x25519_public_key(public_key, private_key);
    return 0;
}

int ct_crypto_kx(uint8_t shared_secret[32], const uint8_t private_key[32],
                 const uint8_t peer_public_key[32]) {
    static const uint8_t zero[32] = {0};
    crypto_x25519(shared_secret, private_key, peer_public_key);
    if (crypto_verify32(shared_secret, zero) == 0) {
        crypto_wipe(shared_secret, 32);
        return -1;
    }
    return 0;
}

int ct_crypto_kdf(uint8_t *output, size_t output_length, const uint8_t *key_material,
                  size_t key_material_length, const uint8_t *salt, size_t salt_length,
                  const uint8_t *context, size_t context_length) {
    if ((!output && output_length) || (!key_material && key_material_length) || salt_length > 64 ||
        (!salt && salt_length) || (!context && context_length))
        return -1;
    uint8_t prk[64];
    if (salt_length)
        crypto_blake2b_keyed(prk, sizeof prk, salt, salt_length, key_material, key_material_length);
    else
        crypto_blake2b(prk, sizeof prk, key_material, key_material_length);
    uint64_t counter = 0;
    while (output_length) {
        size_t block = output_length < 64 ? output_length : 64;
        uint8_t encoded_counter[8], expanded[64];
        for (size_t i = 0; i < sizeof encoded_counter; i++)
            encoded_counter[i] = (uint8_t)(counter >> (i * 8));
        crypto_blake2b_ctx ctx;
        crypto_blake2b_keyed_init(&ctx, sizeof expanded, prk, sizeof prk);
        crypto_blake2b_update(&ctx, encoded_counter, sizeof encoded_counter);
        crypto_blake2b_update(&ctx, context, context_length);
        crypto_blake2b_final(&ctx, expanded);
        memcpy(output, expanded, block);
        crypto_wipe(expanded, sizeof expanded);
        output += block;
        output_length -= block;
        if (counter == UINT64_MAX && output_length) {
            crypto_wipe(prk, sizeof prk);
            return -1;
        }
        counter++;
    }
    crypto_wipe(prk, sizeof prk);
    return 0;
}

int ct_crypto_aead_encrypt(uint8_t *ciphertext, uint8_t tag[16], const uint8_t *plaintext,
                           size_t plaintext_length, const uint8_t *associated_data,
                           size_t associated_data_length, const uint8_t nonce[24],
                           const uint8_t key[32]) {
    crypto_aead_lock(ciphertext, tag, key, nonce, associated_data, associated_data_length,
                     plaintext, plaintext_length);
    return 0;
}

int ct_crypto_aead_decrypt(uint8_t *plaintext, const uint8_t *ciphertext, size_t ciphertext_length,
                           const uint8_t tag[16], const uint8_t *associated_data,
                           size_t associated_data_length, const uint8_t nonce[24],
                           const uint8_t key[32]) {
    return crypto_aead_unlock(plaintext, tag, key, nonce, associated_data, associated_data_length,
                              ciphertext, ciphertext_length);
}

void ct_crypto_hash(uint8_t output[32], const uint8_t *message, size_t message_length) {
    crypto_blake2b(output, 32, message, message_length);
}

void ct_crypto_mac(uint8_t output[32], const uint8_t key[32], const uint8_t *message,
                   size_t message_length) {
    crypto_blake2b_keyed(output, 32, key, 32, message, message_length);
}

void ct_crypto_wipe(void *memory, size_t length) {
    crypto_wipe(memory, length);
}

int ct_crypto_equal(const uint8_t *left, const uint8_t *right, size_t length) {
    if (length == 16)
        return crypto_verify16(left, right) == 0;
    if (length == 32)
        return crypto_verify32(left, right) == 0;
    if (length == 64)
        return crypto_verify64(left, right) == 0;
    uint8_t difference = 0;
    for (size_t i = 0; i < length; i++)
        difference |= left[i] ^ right[i];
    return difference == 0;
}

const char *ct_crypto_backend_name(void) {
    return "Monocypher";
}
const char *ct_crypto_backend_version(void) {
    return "4.0.3";
}

#ifdef CONFIG_FEATURE_KEYGEN
static int write_key(const char *path, const char *prefix, const uint8_t *key, size_t length,
                     int private_file) {
    char encoded[256] = {0};
    size_t prefix_length = strlen(prefix);
    if (prefix_length >= sizeof encoded ||
        b64_encode(encoded + prefix_length, sizeof encoded - prefix_length, key, length)) {
        crypto_wipe(encoded, sizeof encoded);
        return -1;
    }
    memcpy(encoded, prefix, prefix_length);
    size_t encoded_length = strlen(encoded);
    encoded[encoded_length++] = '\n';
#ifdef _WIN32
    (void)private_file;
    FILE *file = fopen(path, "wbx");
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, private_file ? 0600 : 0644);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "wb");
#endif
    if (!file) {
        crypto_wipe(encoded, sizeof encoded);
        return -1;
    }
    int result = fwrite(encoded, 1, encoded_length, file) == encoded_length ? 0 : -1;
    if (fclose(file) != 0)
        result = -1;
    crypto_wipe(encoded, sizeof encoded);
    if (result)
        (void)remove(path);
    return result;
}

int ct_keygen_files(const char *private_path, const char *public_path, char *fingerprint,
                    size_t fingerprint_size) {
    uint8_t public_key[32], private_key[64];
    if (ct_crypto_sign_keypair(public_key, private_key))
        return -1;
    if (write_key(private_path, SEC_PREFIX, private_key, sizeof private_key, 1)) {
        crypto_wipe(private_key, sizeof private_key);
        return -1;
    }
    if (write_key(public_path, PUB_PREFIX, public_key, sizeof public_key, 0)) {
        (void)remove(private_path);
        crypto_wipe(private_key, sizeof private_key);
        return -1;
    }
    uint8_t hash[32];
    char hex[65];
    crypto_blake2b(hash, sizeof hash, public_key, sizeof public_key);
    hex_encode(hex, hash, sizeof hash);
    int written = snprintf(fingerprint, fingerprint_size, "BLAKE2b-256:%s", hex);
    crypto_wipe(private_key, sizeof private_key);
    crypto_wipe(hash, sizeof hash);
    if (written <= 0 || (size_t)written >= fingerprint_size) {
        (void)remove(private_path);
        (void)remove(public_path);
        return -1;
    }
    return 0;
}
#endif

static int load_key(const char *path, const char *prefix, uint8_t *output, size_t expected,
                    int private_file) {
#ifdef _WIN32
    (void)private_file;
    FILE *file = fopen(path, "rb");
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    struct stat status;
    FILE *file = NULL;
    if (fd >= 0 && fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        (!private_file || (status.st_mode & 077) == 0))
        file = fdopen(fd, "rb");
    if (!file && fd >= 0)
        close(fd);
#endif
    if (!file)
        return -1;
    char encoded[512];
    size_t length = fread(encoded, 1, sizeof encoded - 1, file);
    int read_error = ferror(file);
    int truncated = length == sizeof encoded - 1 && !feof(file);
    fclose(file);
    if (read_error || truncated) {
        crypto_wipe(encoded, sizeof encoded);
        return -1;
    }
    encoded[length] = 0;
    if (length && encoded[length - 1] == '\n') {
        encoded[--length] = 0;
        if (length && encoded[length - 1] == '\r')
            encoded[--length] = 0;
    }
    size_t prefix_length = strlen(prefix);
    int result = -1;
    if (length > prefix_length && !memcmp(encoded, prefix, prefix_length))
        result = b64_decode(output, expected, encoded + prefix_length, length - prefix_length);
    crypto_wipe(encoded, sizeof encoded);
    if (result)
        crypto_wipe(output, expected);
    return result;
}

int ct_load_public_key(const char *path, uint8_t key[32]) {
    return load_key(path, PUB_PREFIX, key, 32, 0);
}
int ct_load_private_key(const char *path, uint8_t key[64]) {
    return load_key(path, SEC_PREFIX, key, 64, 1);
}
#ifdef CONFIG_FEATURE_FINGERPRINT
int ct_fingerprint_file(const char *path, char *output, size_t output_size) {
    uint8_t key[32], hash[32];
    char hex[65];
    if (ct_load_public_key(path, key))
        return -1;
    crypto_blake2b(hash, sizeof hash, key, sizeof key);
    hex_encode(hex, hash, sizeof hash);
    crypto_wipe(hash, sizeof hash);
    int written = snprintf(output, output_size, "BLAKE2b-256:%s", hex);
    return written > 0 && (size_t)written < output_size ? 0 : -1;
}
#endif

int ct_ed_sign(uint8_t signature[64], const uint8_t *message, size_t length,
               const uint8_t private_key[64]) {
    return ct_crypto_sign(signature, private_key, message, length);
}
int ct_ed_verify(const uint8_t signature[64], const uint8_t *message, size_t length,
                 const uint8_t public_key[32]) {
    return ct_crypto_verify(signature, public_key, message, length);
}
int ct_x_keypair(uint8_t public_key[32], uint8_t private_key[32]) {
    return ct_crypto_kx_keypair(public_key, private_key);
}
int ct_x_shared(uint8_t shared[32], const uint8_t private_key[32], const uint8_t public_key[32]) {
    return ct_crypto_kx(shared, private_key, public_key);
}

static int derive(const uint8_t material[32], const uint8_t salt[32], const char *label,
                  uint8_t *output, size_t length) {
    return ct_crypto_kdf(output, length, material, 32, salt, 32, (const uint8_t *)label,
                         strlen(label));
}

int ct_derive_session(const uint8_t shared[32], const uint8_t *transcript, size_t transcript_length,
                      ct_session_keys *keys) {
    uint8_t salt[32], session_id[8];
    crypto_blake2b(salt, sizeof salt, transcript, transcript_length);
    int result =
        derive(shared, salt, "ctunnel-v3/control-c2s-key", keys->control_c2s, 32) ||
        derive(shared, salt, "ctunnel-v3/control-s2c-key", keys->control_s2c, 32) ||
        derive(shared, salt, "ctunnel-v3/control-c2s-nonce", keys->nonce_c2s, CT_NONCE_BASE) ||
        derive(shared, salt, "ctunnel-v3/control-s2c-nonce", keys->nonce_s2c, CT_NONCE_BASE) ||
        derive(shared, salt, "ctunnel-v3/work-auth", keys->work_auth_key, 32) ||
        derive(shared, salt, "ctunnel-v3/stream-auth", keys->stream_auth_key, 32) ||
        derive(shared, salt, "ctunnel-v3/session-id", session_id, sizeof session_id);
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    result = result || derive(shared, salt, "ctunnel-v3/data-master", keys->data_master, 32);
#endif
    keys->session_id = ct_get_u64(session_id);
    if (!keys->session_id)
        keys->session_id = 1;
    crypto_wipe(session_id, sizeof session_id);
    crypto_wipe(salt, sizeof salt);
    return result ? -1 : 0;
}

#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
int ct_derive_data(const uint8_t master[32], uint64_t stream, const uint8_t random[32],
                   const uint8_t work_id[32], const char *service_id, ct_enc_mode mode, int dir,
                   uint8_t key[32], uint8_t nonce_base[CT_NONCE_BASE]) {
    uint8_t context[192] = {0};
    size_t service_length = strlen(service_id);
    if (service_length > CT_MAX_SERVICE_ID)
        return -1;
    size_t offset = 0;
    memcpy(context + offset, "ctunnel-v3/data-stream", 22);
    offset += 22;
    ct_put_u64(context + offset, stream);
    offset += 8;
    memcpy(context + offset, random, 32);
    offset += 32;
    memcpy(context + offset, work_id, 32);
    offset += 32;
    ct_put_u16(context + offset, (uint16_t)service_length);
    offset += 2;
    memcpy(context + offset, service_id, service_length);
    offset += service_length;
    context[offset++] = (uint8_t)mode;
    context[offset++] = (uint8_t)dir;
    const char *key_label =
        dir ? "ctunnel-v3/data-stream-s2c-key" : "ctunnel-v3/data-stream-c2s-key";
    const char *nonce_label =
        dir ? "ctunnel-v3/data-stream-s2c-nonce" : "ctunnel-v3/data-stream-c2s-nonce";
    uint8_t salt[32];
    crypto_blake2b(salt, sizeof salt, context, offset);
    int result = derive(master, salt, key_label, key, 32) ||
                 derive(master, salt, nonce_label, nonce_base, CT_NONCE_BASE);
    crypto_wipe(salt, sizeof salt);
    crypto_wipe(context, sizeof context);
    return result ? -1 : 0;
}
#endif

int ct_cipher_available(ct_cipher cipher) {
    return cipher == CT_CIPHER_CHACHA;
}

static void make_nonce(uint8_t nonce[24], const uint8_t base[CT_NONCE_BASE], uint64_t sequence) {
    memcpy(nonce, base, CT_NONCE_BASE);
    ct_put_u64(nonce + CT_NONCE_BASE, sequence);
}
#ifdef CONFIG_FEATURE_TEST_HOOKS
typedef struct {
    uint8_t key_fingerprint[16];
    uint8_t nonce[CT_CRYPTO_AEAD_NONCE_SIZE];
} nonce_observation;
static nonce_observation nonce_observations[4096];
static size_t nonce_observation_count;

void ct_test_nonce_tracker_reset(void) {
    ct_secure_zero(nonce_observations, sizeof nonce_observations);
    nonce_observation_count = 0;
}

static int record_test_nonce(const uint8_t key[32], const uint8_t nonce[24]) {
    uint8_t fingerprint[32];
    ct_crypto_hash(fingerprint, key, 32);
    for (size_t i = 0; i < nonce_observation_count; i++)
        if (ct_crypto_equal(nonce_observations[i].key_fingerprint, fingerprint, 16) &&
            ct_crypto_equal(nonce_observations[i].nonce, nonce, 24)) {
            ct_secure_zero(fingerprint, sizeof fingerprint);
            return -1;
        }
    if (nonce_observation_count == sizeof nonce_observations / sizeof nonce_observations[0]) {
        ct_secure_zero(fingerprint, sizeof fingerprint);
        return -1;
    }
    memcpy(nonce_observations[nonce_observation_count].key_fingerprint, fingerprint, 16);
    memcpy(nonce_observations[nonce_observation_count].nonce, nonce, 24);
    nonce_observation_count++;
    ct_secure_zero(fingerprint, sizeof fingerprint);
    return 0;
}
#endif

int ct_aead_encrypt(ct_cipher cipher, const uint8_t key[32], const uint8_t base[CT_NONCE_BASE],
                    uint64_t sequence, const uint8_t *associated_data,
                    size_t associated_data_length, const uint8_t *plaintext,
                    size_t plaintext_length, uint8_t *output, size_t *output_length) {
    if (!ct_cipher_available(cipher) || plaintext_length > SIZE_MAX - CT_AEAD_TAG)
        return -1;
    uint8_t nonce[24];
    make_nonce(nonce, base, sequence);
#ifdef CONFIG_FEATURE_TEST_HOOKS
    if (record_test_nonce(key, nonce))
        return -1;
#endif
    if (ct_crypto_aead_encrypt(output, output + plaintext_length, plaintext, plaintext_length,
                               associated_data, associated_data_length, nonce, key))
        return -1;
    *output_length = plaintext_length + CT_AEAD_TAG;
    return 0;
}

int ct_aead_decrypt(ct_cipher cipher, const uint8_t key[32], const uint8_t base[CT_NONCE_BASE],
                    uint64_t sequence, const uint8_t *associated_data,
                    size_t associated_data_length, const uint8_t *input, size_t input_length,
                    uint8_t *output, size_t *output_length) {
    if (!ct_cipher_available(cipher) || input_length < CT_AEAD_TAG)
        return -1;
    size_t ciphertext_length = input_length - CT_AEAD_TAG;
    uint8_t nonce[24];
    make_nonce(nonce, base, sequence);
    if (ct_crypto_aead_decrypt(output, input, ciphertext_length, input + ciphertext_length,
                               associated_data, associated_data_length, nonce, key))
        return -1;
    *output_length = ciphertext_length;
    return 0;
}
