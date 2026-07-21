#include "ctunnel.h"
#include "ctunnel/crypto.h"
#include "net/net.h"
#include "net/proxy_protocol.h"
#include "net/relay.h"
#include "platform/platform.h"
#include "protocol/protocol.h"
#include "util/log.h"
#include "util/ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static int fails = 0;
#define T(x)                                                                                       \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

static int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static void hex(uint8_t *out, const char *text, size_t length) {
    T(strlen(text) == length * 2);
    for (size_t i = 0; i < length; i++) {
        int high = nibble(text[i * 2]), low = nibble(text[i * 2 + 1]);
        T(high >= 0 && low >= 0);
        out[i] = (uint8_t)((high << 4) | low);
    }
}
static ct_stream_metadata proxy_metadata(uint8_t mode) {
    ct_stream_metadata metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.proxy_protocol = (ct_proxy_protocol_mode)mode;
    metadata.source.family = 4;
    metadata.source.addr[0] = 192;
    metadata.source.addr[1] = 0;
    metadata.source.addr[2] = 2;
    metadata.source.addr[3] = 10;
    metadata.source.port = 12345;
    metadata.destination.family = 4;
    metadata.destination.addr[0] = 198;
    metadata.destination.addr[1] = 51;
    metadata.destination.addr[2] = 100;
    metadata.destination.addr[3] = 20;
    metadata.destination.port = 8443;
    return metadata;
}

static void test_frame(void) {
    ct_frame_header h = {CT_MSG_PING, CT_FLAG_ENCRYPTED, 123, 0x1122334455667788ULL, 4, 9}, q;
    uint8_t b[CT_FRAME_HEADER_SIZE];
    T(ct_frame_header_encode(&h, b) == 0);
    T(ct_frame_header_decode(b, &q) == 0);
    T(q.type == h.type && q.payload_len == 123 && q.session_id == h.session_id && q.sequence == 9);
    uint8_t expected[CT_FRAME_HEADER_SIZE];
    hex(expected, "4354554e031e01240000007b112233445566778800000000000000040000000000000009",
        sizeof expected);
    T(!memcmp(b, expected, sizeof b));
    b[0] ^= 1;
    T(ct_frame_header_decode(b, &q) != 0);
    b[0] ^= 1;
    b[4] = 2;
    T(ct_frame_header_decode(b, &q) != 0);
    memset(b, 0, sizeof b);
    ct_put_u32(b, CT_MAGIC);
    b[4] = CT_PROTOCOL_VERSION;
    b[5] = 99;
    b[7] = CT_FRAME_HEADER_SIZE;
    T(ct_frame_header_decode(b, &q) != 0);
}

static void test_proxy_protocol_vectors(void) {
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL
    ct_stream_metadata metadata = proxy_metadata(CT_PROXY_PROTOCOL_V1);
    uint8_t out[CT_PROXY_PROTOCOL_MAX_HEADER];
    size_t n = 0;
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL_V1
    const char expected_v1[] = "PROXY TCP4 192.0.2.10 198.51.100.20 12345 8443\r\n";
    T(ct_proxy_protocol_build_v1(out, sizeof out, &n, &metadata) == 0);
    T(n == sizeof expected_v1 - 1 && !memcmp(out, expected_v1, n));
    memset(&metadata, 0, sizeof metadata);
    metadata.proxy_protocol = CT_PROXY_PROTOCOL_V1;
    metadata.source.family = 6;
    metadata.destination.family = 6;
    T(inet_pton(AF_INET6, "2001:db8::1", metadata.source.addr) == 1);
    T(inet_pton(AF_INET6, "2001:db8::2", metadata.destination.addr) == 1);
    metadata.source.port = 12345;
    metadata.destination.port = 8443;
    const char expected_v1_ipv6[] = "PROXY TCP6 2001:db8::1 2001:db8::2 12345 8443\r\n";
    T(ct_proxy_protocol_build_v1(out, sizeof out, &n, &metadata) == 0);
    T(n == sizeof expected_v1_ipv6 - 1 && !memcmp(out, expected_v1_ipv6, n));
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL_V2
    metadata = proxy_metadata(CT_PROXY_PROTOCOL_V2);
    uint8_t expected_v2_ipv4[CT_PROXY_PROTOCOL_V2_IPV4_HEADER_SIZE];
    hex(expected_v2_ipv4, "0d0a0d0a000d0a515549540a2111000cc000020ac6336414303920fb",
        sizeof expected_v2_ipv4);
    T(ct_proxy_protocol_build_v2(out, sizeof out, &n, &metadata) == 0);
    T(n == sizeof expected_v2_ipv4 && !memcmp(out, expected_v2_ipv4, n));

    memset(&metadata, 0, sizeof metadata);
    metadata.proxy_protocol = CT_PROXY_PROTOCOL_V2;
    metadata.source.family = 6;
    metadata.destination.family = 6;
    T(inet_pton(AF_INET6, "2001:db8::1", metadata.source.addr) == 1);
    T(inet_pton(AF_INET6, "2001:db8::2", metadata.destination.addr) == 1);
    metadata.source.port = 12345;
    metadata.destination.port = 8443;
    uint8_t expected_v2_ipv6[CT_PROXY_PROTOCOL_V2_IPV6_HEADER_SIZE];
    hex(expected_v2_ipv6,
        "0d0a0d0a000d0a515549540a21210024"
        "20010db8000000000000000000000001"
        "20010db8000000000000000000000002"
        "303920fb",
        sizeof expected_v2_ipv6);
    T(ct_proxy_protocol_build_v2(out, sizeof out, &n, &metadata) == 0);
    T(n == sizeof expected_v2_ipv6 && !memcmp(out, expected_v2_ipv6, n));
#endif
#endif
}

static void test_ring(void) {
    ct_ring r;
    uint8_t x[8] = {0, 1, 2, 3, 4, 5, 6, 7}, y[8];
    T(ct_ring_init(&r, 8) == 0);
    T(ct_ring_write(&r, x, 6) == 6);
    T(ct_ring_read(&r, y, 4) == 4);
    T(!memcmp(x, y, 4));
    T(ct_ring_write(&r, x + 6, 2) == 2);
    T(ct_ring_read(&r, y, 4) == 4);
    T(y[0] == 4 && y[1] == 5 && y[2] == 6 && y[3] == 7);
    ct_ring_free(&r);
}

static void test_blake2b(void) {
    uint8_t got[32], want[32];
    hex(want, "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8", 32);
    ct_crypto_hash(got, (const uint8_t *)"", 0);
    T(ct_crypto_equal(got, want, 32));
}

/* Official Monocypher EdDSA-BLAKE2b vectors (empty message). */
static void test_signature(void) {
    uint8_t seed[32], pk[32], wrong_pk[32], sk[64], signature[64], want[64];
    const uint8_t empty[] = "";
    hex(seed, "e4e4c4054fe35a75d9c0f679ad8770d8227e68e4c1e68ce67ee88e6be251a207", 32);
    hex(pk, "61435d557c6bedda3b9d652b98982c227ffedb203fc2357cabe8075508f4e6f0", 32);
    hex(want,
        "39ce6ca681872307882d8ab523fbc11aafc0f1fe80295d6145cd7e1af11a5595"
        "c55293991b9c8af9d88e8a326df9281f3a3c42c7d2a371025972d29be89f650f",
        64);
    memcpy(sk, seed, 32);
    memcpy(sk + 32, pk, 32);
    T(ct_crypto_sign(signature, sk, empty, 0) == 0);
    T(ct_crypto_equal(signature, want, 64));
    T(ct_crypto_verify(signature, pk, empty, 0) == 0);
    signature[0] ^= 1;
    T(ct_crypto_verify(signature, pk, empty, 0) != 0);
    signature[0] ^= 1;
    memcpy(wrong_pk, pk, 32);
    wrong_pk[0] ^= 1;
    T(ct_crypto_verify(signature, wrong_pk, empty, 0) != 0);
    T(ct_crypto_verify(signature, pk, (const uint8_t *)"x", 1) != 0);

    const uint8_t server_context[] = "ctunnel server handshake v3";
    const uint8_t client_context[] = "ctunnel client handshake v3";
    T(ct_crypto_sign(signature, sk, server_context, sizeof server_context - 1) == 0);
    T(ct_crypto_verify(signature, pk, client_context, sizeof client_context - 1) != 0);
}

static void test_x25519(void) {
    uint8_t alice_sk[32], bob_sk[32], bob_pk[32], want[32], a_shared[32], b_shared[32],
        alice_pk[32];
    uint8_t zero[32] = {0};
    hex(alice_sk, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", 32);
    hex(bob_sk, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", 32);
    hex(bob_pk, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
    hex(want, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    T(ct_crypto_kx(a_shared, alice_sk, bob_pk) == 0);
    T(ct_crypto_equal(a_shared, want, 32));
    /* RFC 7748 Alice public key. */
    hex(alice_pk, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);
    T(ct_crypto_kx(b_shared, bob_sk, alice_pk) == 0);
    T(ct_crypto_equal(a_shared, b_shared, 32));
    T(ct_crypto_kx(a_shared, alice_sk, zero) != 0);

    uint8_t public1[32], private1[32], public2[32], private2[32], public3[32], private3[32];
    uint8_t shared12[32], shared21[32], shared13[32];
    T(ct_crypto_kx_keypair(public1, private1) == 0);
    T(ct_crypto_kx_keypair(public2, private2) == 0);
    T(ct_crypto_kx_keypair(public3, private3) == 0);
    T(ct_crypto_kx(shared12, private1, public2) == 0);
    T(ct_crypto_kx(shared21, private2, public1) == 0);
    T(ct_crypto_kx(shared13, private1, public3) == 0);
    T(ct_crypto_equal(shared12, shared21, 32));
    T(!ct_crypto_equal(shared12, shared13, 32));
}

static void test_kdf(void) {
    uint8_t material[32] = {1}, salt[32] = {2}, a[80], b[80], c[80], prefix[32];
    T(ct_crypto_kdf(a, sizeof a, material, sizeof material, salt, sizeof salt,
                    (const uint8_t *)"context-a", 9) == 0);
    T(ct_crypto_kdf(b, sizeof b, material, sizeof material, salt, sizeof salt,
                    (const uint8_t *)"context-a", 9) == 0);
    T(ct_crypto_equal(a, b, sizeof a));
    T(ct_crypto_kdf(prefix, sizeof prefix, material, sizeof material, salt, sizeof salt,
                    (const uint8_t *)"context-a", 9) == 0);
    T(ct_crypto_equal(a, prefix, sizeof prefix));
    T(ct_crypto_kdf(c, sizeof c, material, sizeof material, salt, sizeof salt,
                    (const uint8_t *)"context-b", 9) == 0);
    T(!ct_crypto_equal(a, c, sizeof a));
    salt[0] ^= 1;
    T(ct_crypto_kdf(c, sizeof c, material, sizeof material, salt, sizeof salt,
                    (const uint8_t *)"context-a", 9) == 0);
    T(!ct_crypto_equal(a, c, sizeof a));
    uint8_t vector_material[32] = {1}, vector_salt[32] = {2}, vector[32], expected[32];
    hex(expected, "abca5d44eb7d5872ee45ff9596ce39ee6f3a10f89916fc1ecd25a4ffd722c6d3", 32);
    T(ct_crypto_kdf(vector, sizeof vector, vector_material, sizeof vector_material, vector_salt,
                    sizeof vector_salt, (const uint8_t *)"ctunnel-v3/control-c2s-key", 26) == 0);
    T(ct_crypto_equal(vector, expected, sizeof vector));
}

static void test_aead_vector(void) {
    uint8_t key[32], nonce[24], tag[16], want[16], dummy = 0;
    hex(key, "e4e4c4054fe35a75d9c0f679ad8770d8227e68e4c1e68ce67ee88e6be251a207", 32);
    hex(nonce, "48b3753cff3a6d990163e6b60da1e4e5d6a2df78c16c96a5", 24);
    hex(want, "b5ed4c7e63a144f105dbe2b039c7e805", 16);
    T(ct_crypto_aead_encrypt(&dummy, tag, &dummy, 0, &dummy, 0, nonce, key) == 0);
    T(ct_crypto_equal(tag, want, sizeof tag));
    T(ct_crypto_aead_decrypt(&dummy, &dummy, 0, tag, &dummy, 0, nonce, key) == 0);
}

static void test_aead(void) {
    uint8_t key[32] = {0}, base[CT_NONCE_BASE] = {1, 2, 3, 4};
    uint8_t ad[3] = {7, 8, 9}, bad_ad[3] = {7, 8, 8}, plain[5] = {1, 2, 3, 4, 5};
    uint8_t box[64], saved[64], output[64];
    uint8_t other_nonce_box[64];
    size_t box_length = 0, output_length = 0;
    T(ct_aead_encrypt(CT_CIPHER_CHACHA, key, base, 1, ad, sizeof ad, plain, sizeof plain, box,
                      &box_length) == 0);
    T(box_length == sizeof plain + CT_AEAD_TAG);
    memcpy(saved, box, box_length);
    size_t other_length = 0;
    T(ct_aead_encrypt(CT_CIPHER_CHACHA, key, base, 2, ad, sizeof ad, plain, sizeof plain,
                      other_nonce_box, &other_length) == 0);
    T(other_length == box_length && !ct_crypto_equal(saved, other_nonce_box, box_length));
    T(ct_aead_decrypt(CT_CIPHER_CHACHA, key, base, 1, ad, sizeof ad, box, box_length, output,
                      &output_length) == 0);
    T(output_length == sizeof plain && !memcmp(plain, output, sizeof plain));
    box[0] ^= 1;
    T(ct_aead_decrypt(CT_CIPHER_CHACHA, key, base, 1, ad, sizeof ad, box, box_length, output,
                      &output_length) != 0);
    memcpy(box, saved, box_length);
    box[box_length - 1] ^= 1;
    T(ct_aead_decrypt(CT_CIPHER_CHACHA, key, base, 1, ad, sizeof ad, box, box_length, output,
                      &output_length) != 0);
    memcpy(box, saved, box_length);
    T(ct_aead_decrypt(CT_CIPHER_CHACHA, key, base, 1, bad_ad, sizeof bad_ad, box, box_length,
                      output, &output_length) != 0);
    T(ct_aead_decrypt(CT_CIPHER_CHACHA, key, base, 2, ad, sizeof ad, box, box_length, output,
                      &output_length) != 0);
    T(ct_aead_encrypt((ct_cipher)2, key, base, 1, ad, sizeof ad, plain, sizeof plain, box,
                      &box_length) != 0);
}

#ifdef CONFIG_FEATURE_TEST_HOOKS
static void test_nonce_reuse_hook(void) {
    uint8_t key[32] = {7}, base[CT_NONCE_BASE] = {8}, plaintext[1] = {9}, output[32];
    size_t output_length;
    ct_test_nonce_tracker_reset();
    T(ct_aead_encrypt(CT_CIPHER_CHACHA, key, base, 1, NULL, 0, plaintext, sizeof plaintext, output,
                      &output_length) == 0);
    T(ct_aead_encrypt(CT_CIPHER_CHACHA, key, base, 1, NULL, 0, plaintext, sizeof plaintext, output,
                      &output_length) != 0);
    ct_test_nonce_tracker_reset();
}
#endif

static void test_session_keys(void) {
    uint8_t shared[32] = {1}, transcript[32] = {2};
    ct_session_keys a, b;
    T(ct_derive_session(shared, transcript, sizeof transcript, &a) == 0);
    T(ct_derive_session(shared, transcript, sizeof transcript, &b) == 0);
    T(!memcmp(&a, &b, sizeof a));
    T(!ct_crypto_equal(a.control_c2s, a.control_s2c, 32));
    T(!ct_crypto_equal(a.control_c2s, a.data_master, 32));
    T(!ct_crypto_equal(a.data_master, a.work_auth_key, 32));
    T(!ct_crypto_equal(a.work_auth_key, a.stream_auth_key, 32));
    uint8_t key1[32], key2[32], nonce1[CT_NONCE_BASE], nonce2[CT_NONCE_BASE], random[32] = {3},
                                                                              work_id[32] = {4};
    T(ct_derive_data(a.data_master, 9, random, work_id, "service", CT_ENC_REQUIRED, 0, key1,
                     nonce1) == 0);
    T(ct_derive_data(a.data_master, 9, random, work_id, "service", CT_ENC_REQUIRED, 1, key2,
                     nonce2) == 0);
    T(!ct_crypto_equal(key1, key2, 32));
    T(!ct_crypto_equal(nonce1, nonce2, CT_NONCE_BASE));
    T(ct_derive_data(a.data_master, 10, random, work_id, "service", CT_ENC_REQUIRED, 0, key2,
                     nonce2) == 0);
    T(!ct_crypto_equal(key1, key2, 32));
    transcript[0] ^= 1;
    T(ct_derive_session(shared, transcript, sizeof transcript, &b) == 0);
    T(!ct_crypto_equal(a.control_c2s, b.control_c2s, 32));
}

static void test_random_and_wipe(void) {
    uint8_t a[32], b[32], wipe[32];
    memset(wipe, 0xa5, sizeof wipe);
    T(ct_crypto_random(a, sizeof a) == 0);
    T(ct_crypto_random(b, sizeof b) == 0);
    T(!ct_crypto_equal(a, b, sizeof a));
    T(ct_crypto_equal(a, a, sizeof a));
    ct_crypto_wipe(wipe, sizeof wipe);
    uint8_t zero[32] = {0};
    T(ct_crypto_equal(wipe, zero, sizeof wipe));
}

static void test_authorization(void) {
    ct_authorized_client a;
    memset(&a, 0, sizeof a);
    strcpy(a.allow_addr, "::");
    a.ports[0] = (ct_port_range){2000, 2999};
    a.ports[1] = (ct_port_range){8443, 8443};
    a.port_count = 2;
    T(ct_authorized_port(&a, "::", 2222));
    T(ct_authorized_port(&a, "::", 8443));
    T(!ct_authorized_port(&a, "::", 3000));
    T(!ct_authorized_port(&a, "127.0.0.1", 2222));
}

#ifndef _WIN32
static void send_work_bind(int socket_fd, const ct_control *control, uint64_t sequence) {
    uint8_t payload[64] = {0}, authenticated[160], mac[32];
    const char label[] = "ctunnel-v3/work-bind/client-to-server";
    ct_put_u64(payload, sequence);
    for (size_t i = 8; i < 32; i++)
        payload[i] = (uint8_t)i;
    size_t offset = 0, client_length = strlen(control->client_id);
    memcpy(authenticated + offset, label, sizeof label - 1);
    offset += sizeof label - 1;
    ct_put_u64(authenticated + offset, control->keys.session_id);
    offset += 8;
    ct_put_u16(authenticated + offset, (uint16_t)client_length);
    offset += 2;
    memcpy(authenticated + offset, control->client_id, client_length);
    offset += client_length;
    authenticated[offset++] = CT_MSG_WORK_CONNECTION_BIND;
    memcpy(authenticated + offset, payload, 32);
    offset += 32;
    ct_crypto_mac(mac, control->keys.work_auth_key, authenticated, offset);
    memcpy(payload + 32, mac, sizeof mac);
    T(ct_plain_send(socket_fd, CT_MSG_WORK_CONNECTION_BIND, control->keys.session_id, 0, payload,
                    sizeof payload, 1000) == 0);
}

static void test_work_replay(void) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        T(0);
        return;
    }
    uint8_t shared[32] = {4}, transcript[32] = {5};
    ct_control server;
    memset(&server, 0, sizeof server);
    T(ct_derive_session(shared, transcript, sizeof transcript, &server.keys) == 0);
    strcpy(server.client_id, "test-client");
    ct_work accepted;

    send_work_bind(sockets[0], &server, 2);
    send_work_bind(sockets[0], &server, 1);
    send_work_bind(sockets[0], &server, 1);
    T(ct_work_accept_bind(sockets[1], &server, &accepted) == 0);
    T(ct_work_accept_bind(sockets[1], &server, &accepted) == 0);
    T(ct_work_accept_bind(sockets[1], &server, &accepted) != 0);
    send_work_bind(sockets[0], &server, 100);
    send_work_bind(sockets[0], &server, 3);
    T(ct_work_accept_bind(sockets[1], &server, &accepted) == 0);
    T(ct_work_accept_bind(sockets[1], &server, &accepted) != 0);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_listener_overlap(void) {
    int first = socket(AF_INET, SOCK_DGRAM, 0);
    int second = socket(AF_INET, SOCK_DGRAM, 0);
    if (first < 0 || second < 0) {
        T(0);
        if (first >= 0)
            close(first);
        if (second >= 0)
            close(second);
        return;
    }
    int one = 1;
    T(setsockopt(first, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0);
    T(setsockopt(second, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0);
    struct sockaddr_in wildcard, specific;
    memset(&wildcard, 0, sizeof wildcard);
    wildcard.sin_family = AF_INET;
    wildcard.sin_addr.s_addr = htonl(INADDR_ANY);
    T(bind(first, (struct sockaddr *)&wildcard, sizeof wildcard) == 0);
    socklen_t length = sizeof wildcard;
    T(getsockname(first, (struct sockaddr *)&wildcard, &length) == 0);
    memset(&specific, 0, sizeof specific);
    specific.sin_family = AF_INET;
    specific.sin_port = wildcard.sin_port;
    specific.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    T(bind(second, (struct sockaddr *)&specific, sizeof specific) == 0);
    T(ct_net_bound_endpoints_overlap(first, second) == 1);
    close(first);
    close(second);
}

static void test_sequence_replay(void) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        T(0);
        return;
    }
    uint8_t shared[32] = {9}, transcript[32] = {8};
    ct_control receiver;
    memset(&receiver, 0, sizeof receiver);
    T(ct_derive_session(shared, transcript, sizeof transcript, &receiver.keys) == 0);
    receiver.fd = sockets[1];
    receiver.cipher = CT_CIPHER_CHACHA;
    receiver.is_client = 0;

    const uint8_t plaintext[] = {1, 2, 3};
    uint8_t header[CT_FRAME_HEADER_SIZE], box[sizeof plaintext + CT_AEAD_TAG];
    size_t box_length = 0;
    ct_frame_header frame = {
        CT_MSG_PING, CT_FLAG_ENCRYPTED, sizeof box, receiver.keys.session_id, 0, 1};
    T(ct_frame_header_encode(&frame, header) == 0);
    T(ct_aead_encrypt(CT_CIPHER_CHACHA, receiver.keys.control_c2s, receiver.keys.nonce_c2s, 1,
                      header, sizeof header, plaintext, sizeof plaintext, box, &box_length) == 0);
    for (int i = 0; i < 2; i++) {
        T(write(sockets[0], header, sizeof header) == (ssize_t)sizeof header);
        T(write(sockets[0], box, box_length) == (ssize_t)box_length);
    }
    uint8_t output[16];
    size_t output_length = 0;
    ct_frame_header received;
    T(ct_control_recv(&receiver, &received, output, sizeof output, &output_length, 1000) == 0);
    T(output_length == sizeof plaintext && !memcmp(output, plaintext, sizeof plaintext));
    T(ct_control_recv(&receiver, &received, output, sizeof output, &output_length, 1000) != 0);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_data_record_replay_rejected(void) {
    int public_side[2], local_side[2], tunnel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, public_side) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, local_side) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, tunnel) != 0) {
        T(0);
        return;
    }
    uint8_t master[32] = {1}, random[32] = {2}, work_id[32] = {3};
    ct_relay server, client;
    T(ct_relay_init(&server, public_side[1], tunnel[0], 0, CT_ENC_REQUIRED, CT_CIPHER_CHACHA,
                    master, 55, random, work_id, "ssh", NULL) == 0);
    T(ct_relay_init(&client, local_side[1], tunnel[1], 1, CT_ENC_REQUIRED, CT_CIPHER_CHACHA, master,
                    55, random, work_id, "ssh", NULL) == 0);
    const uint8_t message[] = "data-record-replay";
    T(send(public_side[0], message, sizeof message, 0) == (ssize_t)sizeof message);
    T(ct_relay_process(&server, public_side[1], 1) == 0);
    T(ct_relay_process(&server, tunnel[0], 2) == 0);
    uint8_t record[12 + sizeof message + CT_AEAD_TAG];
    ssize_t record_length = recv(tunnel[1], record, sizeof record, MSG_WAITALL);
    T(record_length == (ssize_t)sizeof record);
    T(send(tunnel[0], record, sizeof record, 0) == (ssize_t)sizeof record);
    T(ct_relay_process(&client, tunnel[1], 1) == 0);
    T(ct_relay_process(&client, local_side[1], 2) == 0);
    uint8_t received[sizeof message];
    T(recv(local_side[0], received, sizeof received, MSG_WAITALL) == (ssize_t)sizeof received);
    T(!memcmp(received, message, sizeof message));
    T(send(tunnel[0], record, sizeof record, 0) == (ssize_t)sizeof record);
    T(ct_relay_process(&client, tunnel[1], 1) != 0);
    T(recv(local_side[0], received, sizeof received, 0) == 0);
    ct_relay_close(&server);
    close(public_side[0]);
    close(local_side[0]);
}

static void test_stream_ready_header_binding(void) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        T(0);
        return;
    }
    uint8_t shared[32] = {6}, transcript_bytes[32] = {7}, random[32] = {8};
    ct_control control;
    memset(&control, 0, sizeof control);
    T(ct_derive_session(shared, transcript_bytes, sizeof transcript_bytes, &control.keys) == 0);
    strcpy(control.client_id, "test-client");
    ct_work sender = {.fd = sockets[0]}, receiver = {.fd = sockets[1]};
    memset(sender.id, 9, sizeof sender.id);
    memcpy(receiver.id, sender.id, sizeof receiver.id);

    T(ct_stream_ready_send(&sender, &control, "x", CT_ENC_REQUIRED, 42, random, 1) == 0);
    uint8_t frame[CT_FRAME_HEADER_SIZE + 109];
    T(read(sockets[1], frame, sizeof frame) == (ssize_t)sizeof frame);
    ct_put_u64(frame + 12, control.keys.session_id ^ UINT64_C(0x100));
    T(write(sockets[0], frame, sizeof frame) == (ssize_t)sizeof frame);
    int ok = 0;
    T(ct_stream_ready_recv(&receiver, &control, "x", CT_ENC_REQUIRED, 42, random, &ok) != 0);

    close(sockets[0]);
    close(sockets[1]);
}

static void test_stream_binding_roundtrip(void) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        T(0);
        return;
    }
    uint8_t shared[32] = {10}, transcript_bytes[32] = {11}, sent_random[32], received_random[32];
    ct_control control;
    memset(&control, 0, sizeof control);
    T(ct_derive_session(shared, transcript_bytes, sizeof transcript_bytes, &control.keys) == 0);
    strcpy(control.client_id, "test-client");
    ct_work sender = {.fd = sockets[0]}, receiver = {.fd = sockets[1]};
    for (size_t i = 0; i < sizeof sender.id; i++)
        sender.id[i] = (uint8_t)i;
    memcpy(receiver.id, sender.id, sizeof receiver.id);
    ct_stream_metadata metadata = proxy_metadata(CT_PROXY_PROTOCOL_OFF);
    T(ct_start_stream_send(&sender, &control, "ssh", 77, CT_ENC_REQUIRED, &metadata, sent_random) ==
      0);
    char service[CT_MAX_SERVICE_ID + 1];
    uint64_t stream_id = 0;
    ct_enc_mode mode = CT_ENC_DISABLED;
    ct_stream_metadata received_metadata;
    T(ct_start_stream_recv(&receiver, &control, service, sizeof service, &stream_id, &mode,
                           &received_metadata, received_random) == 0);
    T(!strcmp(service, "ssh") && stream_id == 77 && mode == CT_ENC_REQUIRED &&
      !memcmp(sent_random, received_random, sizeof sent_random));
    T(received_metadata.proxy_protocol == CT_PROXY_PROTOCOL_OFF);
    T(ct_stream_ready_send(&receiver, &control, service, mode, stream_id, received_random, 1) == 0);
    int ok = 0;
    T(ct_stream_ready_recv(&sender, &control, "ssh", CT_ENC_REQUIRED, 77, sent_random, &ok) == 0);
    T(ok == 1);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_stream_binding_rejects_wrong_context(void) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        T(0);
        return;
    }
    uint8_t shared[32] = {12}, transcript_bytes[32] = {13}, random[32] = {14};
    ct_control control;
    memset(&control, 0, sizeof control);
    T(ct_derive_session(shared, transcript_bytes, sizeof transcript_bytes, &control.keys) == 0);
    strcpy(control.client_id, "test-client");
    ct_work sender = {.fd = sockets[0]}, receiver = {.fd = sockets[1]};
    memset(sender.id, 1, sizeof sender.id);
    memset(receiver.id, 2, sizeof receiver.id);
    ct_stream_metadata metadata = proxy_metadata(CT_PROXY_PROTOCOL_V2);
    T(ct_start_stream_send(&sender, &control, "ssh", 88, CT_ENC_REQUIRED, &metadata, random) == 0);
    char service[CT_MAX_SERVICE_ID + 1];
    uint64_t stream_id;
    ct_enc_mode mode;
    uint8_t received_random[32];
    ct_stream_metadata received_metadata;
    T(ct_start_stream_recv(&receiver, &control, service, sizeof service, &stream_id, &mode,
                           &received_metadata, received_random) != 0);
    close(sockets[0]);
    close(sockets[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        T(0);
        return;
    }
    sender.fd = sockets[0];
    receiver.fd = sockets[1];
    memcpy(receiver.id, sender.id, sizeof receiver.id);
    T(ct_stream_ready_send(&sender, &control, "ssh", CT_ENC_REQUIRED, 89, random, 1) == 0);
    int ok = 0;
    T(ct_stream_ready_recv(&receiver, &control, "web", CT_ENC_REQUIRED, 89, random, &ok) != 0);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_key_files(void) {
    char directory[] = "/tmp/ctunnel-keys-XXXXXX";
    if (mkdtemp(directory) == NULL) {
        T(0);
        return;
    }
    char private_path[256], public_path[256], fingerprint[96];
    snprintf(private_path, sizeof private_path, "%s/private.key", directory);
    snprintf(public_path, sizeof public_path, "%s/public.key", directory);
    T(ct_keygen_files(private_path, public_path, fingerprint, sizeof fingerprint) == 0);
    T(!strncmp(fingerprint, "BLAKE2b-256:", 12));
    uint8_t private_key[64], public_key[32], signature[64];
    T(ct_load_private_key(private_path, private_key) == 0);
    T(ct_load_public_key(public_path, public_key) == 0);
    T(ct_crypto_sign(signature, private_key, (const uint8_t *)"test", 4) == 0);
    T(ct_crypto_verify(signature, public_key, (const uint8_t *)"test", 4) == 0);
    char fingerprint2[96];
    T(ct_fingerprint_file(public_path, fingerprint2, sizeof fingerprint2) == 0);
    T(!strcmp(fingerprint, fingerprint2));
    struct stat status;
    T(stat(private_path, &status) == 0 && (status.st_mode & 0777) == 0600);
    T(ct_keygen_files(private_path, public_path, fingerprint, sizeof fingerprint) != 0);
    int public_fd = open(public_path, O_WRONLY | O_APPEND);
    T(public_fd >= 0);
    if (public_fd >= 0) {
        T(write(public_fd, " ", 1) == 1);
        close(public_fd);
        T(ct_load_public_key(public_path, public_key) != 0);
    }
    unlink(private_path);
    unlink(public_path);
    rmdir(directory);
}

static void test_config(void) {
    char path[] = "/tmp/ctunnel-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        T(0);
        return;
    }
    const char *config =
        "[common]\nmode=client\nserver_addr=::1\nserver_port=7000\nclient_id=x\nidentity_private_"
        "key=/tmp/x\nserver_public_key=/tmp/y\nallowed_ciphers=xchacha20-poly1305\n"
        "preferred_cipher=xchacha20-poly1305\nheartbeat_interval=2\nheartbeat_timeout=5\n"
        "[ssh]\ntype=tcp\nremote_addr=::1\nremote_port=2222\nlocal_addr=127.0.0.1\n"
        "local_port=22\ndata_encryption=false\n"
        "[web]\ntype=tcp\nremote_addr=::1\nremote_port=8080\nlocal_addr=127.0.0.1\n"
        "local_port=8080\ndata_encryption=true\n";
    T(write(fd, config, strlen(config)) == (ssize_t)strlen(config));
    close(fd);
    ct_config parsed;
    char error[256];
    T(ct_config_load(path, &parsed, error, sizeof error) == 0);
    T(parsed.service_count == 2 && parsed.services[0].remote_port == 2222);
    T(parsed.services[0].encryption == CT_ENC_DISABLED);
    T(parsed.services[1].encryption == CT_ENC_REQUIRED);
    T(chmod(path, 0666) == 0);
    T(ct_config_load(path, &parsed, error, sizeof error) != 0);
    unlink(path);
}

static void test_duplicate_security_config(void) {
    char path[] = "/tmp/ctunnel-duplicate-config-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        T(0);
        return;
    }
    const char *config = "[common]\nmode=client\nmode=client\nserver_addr=::1\nclient_id=x\n"
                         "identity_private_key=/tmp/x\nserver_public_key=/tmp/y\n";
    T(write(fd, config, strlen(config)) == (ssize_t)strlen(config));
    close(fd);
    ct_config parsed;
    char error[256];
    T(ct_config_load(path, &parsed, error, sizeof error) != 0);
    unlink(path);
}

static void test_private_key_permissions_rejected(void) {
    char path[] = "/tmp/ctunnel-private-perms-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        T(0);
        return;
    }
    close(fd);
    T(chmod(path, 0644) == 0);
    ct_config config;
    memset(&config, 0, sizeof config);
    config.mode = CT_MODE_CLIENT;
    config.cipher_mask = 1u << CT_CIPHER_CHACHA;
    config.preferred_cipher = CT_CIPHER_CHACHA;
    config.heartbeat_interval = 2;
    config.heartbeat_timeout = 5;
    config.max_clients = 1;
    config.max_services_per_client = 1;
    config.max_streams_per_client = 1;
    config.max_pending_streams = 1;
    strcpy(config.identity_private_key, path);
    strcpy(config.server_addr, "::1");
    strcpy(config.client_id, "x");
    strcpy(config.server_public_key, "/tmp/server.pub");
    char error[256];
    T(ct_config_validate_security_files(&config, error, sizeof error) != 0);
    unlink(path);
}

static void test_log_file_cap(void) {
    char path[] = "/tmp/ctunnel-log-cap-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        T(0);
        return;
    }
    T(ftruncate(fd, 65530) == 0);
    close(fd);
    T(ct_log_configure(path, 7, 64) == 0);
    ct_log_write(CT_LOG_LEVEL_ERROR, "test", "entry must not cross cap");
    struct stat status;
    T(stat(path, &status) == 0 && status.st_size == 65530);
    T(ct_log_configure("stderr", 7, 64) == 0);
    unlink(path);
}
#endif

int main(void) {
    T(ct_platform_init() == 0);
    T(ct_crypto_init() == 0);
    T(!strcmp(ct_crypto_backend_name(), "Monocypher"));
    T(!strcmp(ct_crypto_backend_version(), "4.0.3"));
    test_frame();
    test_proxy_protocol_vectors();
    test_ring();
    test_blake2b();
    test_signature();
    test_x25519();
    test_kdf();
    test_aead_vector();
    test_aead();
#ifdef CONFIG_FEATURE_TEST_HOOKS
    test_nonce_reuse_hook();
#endif
    test_session_keys();
    test_random_and_wipe();
    test_authorization();
#ifndef _WIN32
    test_work_replay();
    test_listener_overlap();
    test_sequence_replay();
    test_data_record_replay_rejected();
    test_stream_ready_header_binding();
    test_stream_binding_roundtrip();
    test_stream_binding_rejects_wrong_context();
    test_key_files();
    test_config();
    test_duplicate_security_config();
    test_private_key_permissions_rejected();
    test_log_file_cap();
#endif
    ct_platform_cleanup();
    if (fails) {
        fprintf(stderr, "%d test(s) failed\n", fails);
        return 1;
    }
    puts("all unit tests passed");
    return 0;
}
