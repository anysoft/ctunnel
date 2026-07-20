#include "net/net.h"
#include "util/log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#define poll WSAPoll
typedef WSAPOLLFD ct_pollfd;
typedef int ct_socklen;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct pollfd ct_pollfd;
typedef socklen_t ct_socklen;
#endif
static int waitfd(ct_socket fd, int wr, int ms) {
    ct_pollfd p = {fd, (short)(wr ? POLLOUT : POLLIN), 0};
    int r;
    do {
        r = poll(&p, 1, ms);
    } while (r < 0 && errno == EINTR);
    return r > 0 ? 0 : -1;
}
static int socket_send(ct_socket fd, const uint8_t *p, size_t n) {
#ifdef _WIN32
    return send(fd, (const char *)p, (int)n, 0);
#else
    return (int)send(fd, p, n, 0);
#endif
}
static int socket_recv(ct_socket fd, uint8_t *p, size_t n) {
#ifdef _WIN32
    return recv(fd, (char *)p, (int)n, 0);
#else
    return (int)recv(fd, p, n, 0);
#endif
}
static int sendall(ct_socket fd, const uint8_t *p, size_t n, int ms) {
    uint64_t deadline = ct_monotonic_ms() + (uint64_t)ms;
    while (n) {
        size_t chunk = n > 0x7fffffff ? 0x7fffffff : n;
        int z = socket_send(fd, p, chunk);
        if (z > 0) {
            p += z;
            n -= (size_t)z;
            continue;
        }
        if (z < 0 && ct_socket_would_block()) {
            uint64_t now = ct_monotonic_ms();
            if (now >= deadline || waitfd(fd, 1, (int)(deadline - now)))
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}
static int recvall(ct_socket fd, uint8_t *p, size_t n, int ms) {
    uint64_t deadline = ct_monotonic_ms() + (uint64_t)ms;
    while (n) {
        size_t chunk = n > 0x7fffffff ? 0x7fffffff : n;
        int z = socket_recv(fd, p, chunk);
        if (z > 0) {
            p += z;
            n -= (size_t)z;
            continue;
        }
        if (z < 0 && ct_socket_would_block()) {
            uint64_t now = ct_monotonic_ms();
            if (now >= deadline || waitfd(fd, 0, (int)(deadline - now)))
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}
static ct_socket gai_sock(const char *addr, uint16_t port, int passive, int timeout) {
    char ps[8];
    snprintf(ps, sizeof ps, "%u", port);
    struct addrinfo h, *res = NULL, *it;
    memset(&h, 0, sizeof h);
#if defined(CONFIG_FEATURE_IPV4) && defined(CONFIG_FEATURE_IPV6)
    h.ai_family = AF_UNSPEC;
#elif defined(CONFIG_FEATURE_IPV6)
    h.ai_family = AF_INET6;
#else
    h.ai_family = AF_INET;
#endif
    h.ai_socktype = SOCK_STREAM;
    h.ai_protocol = IPPROTO_TCP;
    if (passive)
        h.ai_flags = AI_PASSIVE;
    if (getaddrinfo(addr && *addr ? addr : NULL, ps, &h, &res))
        return CT_INVALID_SOCKET;
    ct_socket fd = CT_INVALID_SOCKET;
    for (it = res; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == CT_INVALID_SOCKET)
            continue;
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#ifdef CONFIG_FEATURE_IPV6
        if (passive && it->ai_family == AF_INET6)
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&one, sizeof one);
#endif
        if (ct_socket_nonblock(fd)) {
            ct_socket_close(fd);
            fd = CT_INVALID_SOCKET;
            continue;
        }
        if (passive) {
            if (bind(fd, it->ai_addr, (ct_socklen)it->ai_addrlen) == 0 && listen(fd, 128) == 0)
                break;
        } else {
            if (connect(fd, it->ai_addr, (ct_socklen)it->ai_addrlen) == 0 ||
                (ct_socket_would_block() && !waitfd(fd, 1, timeout))) {
                int er = 0;
                ct_socklen el = sizeof er;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&er, &el) == 0 && er == 0)
                    break;
            }
        }
        ct_socket_close(fd);
        fd = CT_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (fd != CT_INVALID_SOCKET)
#ifdef CONFIG_FEATURE_TCP_KEEPALIVE_TUNING
        ct_socket_keepalive(fd);
#endif
    return fd;
}
ct_socket ct_net_listen(const char *a, uint16_t p, int backlog) {
    ct_socket fd = gai_sock(a, p, 1, 0);
    if (fd != CT_INVALID_SOCKET && backlog > 0)
        (void)listen(fd, backlog);
    return fd;
}
ct_socket ct_net_connect(const char *a, uint16_t p, int timeout) {
    return gai_sock(a, p, 0, timeout * 1000);
}
ct_socket ct_net_accept(ct_socket l, char *out, size_t n) {
    struct sockaddr_storage ss;
    ct_socklen sl = sizeof ss;
    ct_socket f = accept(l, (struct sockaddr *)&ss, &sl);
    if (f == CT_INVALID_SOCKET)
        return f;
    if (ct_socket_nonblock(f)) {
        ct_socket_close(f);
        return CT_INVALID_SOCKET;
    }
#ifdef CONFIG_FEATURE_TCP_KEEPALIVE_TUNING
    ct_socket_keepalive(f);
#endif
    if (out && n)
        getnameinfo((struct sockaddr *)&ss, sl, out, (ct_socklen)n, NULL, 0, NI_NUMERICHOST);
    return f;
}
int ct_net_bound_endpoints_overlap(ct_socket left, ct_socket right) {
    struct sockaddr_storage a, b;
    ct_socklen a_length = sizeof a, b_length = sizeof b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    if (getsockname(left, (struct sockaddr *)&a, &a_length) != 0 ||
        getsockname(right, (struct sockaddr *)&b, &b_length) != 0 || a.ss_family != b.ss_family)
        return 0;
    if (a.ss_family == AF_INET) {
        const struct sockaddr_in *aa = (const struct sockaddr_in *)&a;
        const struct sockaddr_in *bb = (const struct sockaddr_in *)&b;
        if (aa->sin_port != bb->sin_port)
            return 0;
        return aa->sin_addr.s_addr == htonl(INADDR_ANY) ||
               bb->sin_addr.s_addr == htonl(INADDR_ANY) ||
               aa->sin_addr.s_addr == bb->sin_addr.s_addr;
    }
#ifdef CONFIG_FEATURE_IPV6
    if (a.ss_family == AF_INET6) {
        const struct sockaddr_in6 *aa = (const struct sockaddr_in6 *)&a;
        const struct sockaddr_in6 *bb = (const struct sockaddr_in6 *)&b;
        if (aa->sin6_port != bb->sin6_port)
            return 0;
        static const struct in6_addr any = IN6ADDR_ANY_INIT;
        int a_any = memcmp(&aa->sin6_addr, &any, sizeof any) == 0;
        int b_any = memcmp(&bb->sin6_addr, &any, sizeof any) == 0;
        return a_any || b_any ||
               (aa->sin6_scope_id == bb->sin6_scope_id &&
                memcmp(&aa->sin6_addr, &bb->sin6_addr, sizeof aa->sin6_addr) == 0);
    }
#endif
    return 0;
}
int ct_plain_send(ct_socket fd, uint8_t type, uint64_t session, uint64_t stream, const uint8_t *p,
                  size_t n, int ms) {
    if (n > CT_MAX_FRAME_PAYLOAD)
        return -1;
    ct_frame_header h = {type, 0, (uint32_t)n, session, stream, 0};
    uint8_t b[CT_FRAME_HEADER_SIZE];
    if (ct_frame_header_encode(&h, b) || sendall(fd, b, sizeof b, ms) || sendall(fd, p, n, ms))
        return -1;
    return 0;
}
int ct_plain_recv(ct_socket fd, ct_frame_header *h, uint8_t *p, size_t cap, size_t *n, int ms) {
    uint8_t b[CT_FRAME_HEADER_SIZE];
    if (recvall(fd, b, sizeof b, ms) || ct_frame_header_decode(b, h) || h->payload_len > cap ||
        h->flags)
        return -1;
    if (recvall(fd, p, h->payload_len, ms))
        return -1;
    *n = h->payload_len;
    return 0;
}
int ct_frame_available(ct_socket fd, size_t max_payload, uint8_t expected_flags,
                       ct_frame_header *h) {
    uint8_t header[CT_FRAME_HEADER_SIZE];
#ifdef _WIN32
    int received = recv(fd, (char *)header, sizeof header, MSG_PEEK);
#else
    int received = (int)recv(fd, header, sizeof header, MSG_PEEK);
#endif
    if (received == 0)
        return -1;
    if (received < 0)
        return ct_socket_would_block() ? 0 : -1;
    if (received < (int)sizeof header)
        return 0;
    if (ct_frame_header_decode(header, h) || h->flags != expected_flags ||
        h->payload_len > max_payload)
        return -1;
    size_t total = CT_FRAME_HEADER_SIZE + (size_t)h->payload_len;
    if (total < (size_t)h->payload_len)
        return -1;
#ifdef _WIN32
    u_long queued = 0;
    if (ioctlsocket(fd, FIONREAD, &queued) != 0)
        return -1;
    return (uint64_t)queued >= (uint64_t)total ? 1 : 0;
#else
    int queued = 0;
    if (ioctl(fd, FIONREAD, &queued) != 0)
        return -1;
    return queued >= 0 && (size_t)queued >= total ? 1 : 0;
#endif
}
int ct_plain_frame_available(ct_socket fd, size_t max_payload, ct_frame_header *h) {
    return ct_frame_available(fd, max_payload, 0, h);
}
int ct_control_send(ct_control *c, uint8_t type, uint64_t stream, const uint8_t *p, size_t n,
                    int ms) {
    if (c->tx_seq == UINT64_MAX || n > CT_MAX_FRAME_PAYLOAD - CT_AEAD_TAG)
        return -1;
    uint64_t seq = ++c->tx_seq;
    ct_frame_header h = {
        type, CT_FLAG_ENCRYPTED, (uint32_t)(n + CT_AEAD_TAG), c->keys.session_id, stream, seq};
    uint8_t head[CT_FRAME_HEADER_SIZE], *box = malloc(n + CT_AEAD_TAG);
    size_t z = 0;
    if (!box || ct_frame_header_encode(&h, head)) {
        free(box);
        return -1;
    }
    const uint8_t *k = c->is_client ? c->keys.control_c2s : c->keys.control_s2c;
    const uint8_t *nb = c->is_client ? c->keys.nonce_c2s : c->keys.nonce_s2c;
    int rc = ct_aead_encrypt(c->cipher, k, nb, seq, head, sizeof head, p, n, box, &z) ||
             sendall(c->fd, head, sizeof head, ms) || sendall(c->fd, box, z, ms);
    free(box);
    if (!rc)
        c->last_tx_ms = ct_monotonic_ms();
    return rc ? -1 : 0;
}
int ct_control_recv(ct_control *c, ct_frame_header *h, uint8_t *p, size_t cap, size_t *n, int ms) {
    uint8_t head[CT_FRAME_HEADER_SIZE];
    if (recvall(c->fd, head, sizeof head, ms) || ct_frame_header_decode(head, h) ||
        h->flags != CT_FLAG_ENCRYPTED || h->session_id != c->keys.session_id ||
        c->rx_seq == UINT64_MAX || h->sequence != c->rx_seq + 1 || h->payload_len < CT_AEAD_TAG ||
        h->payload_len > cap + CT_AEAD_TAG)
        return -1;
    uint8_t *box = malloc(h->payload_len);
    if (!box)
        return -1;
    if (recvall(c->fd, box, h->payload_len, ms)) {
        free(box);
        return -1;
    }
    const uint8_t *k = c->is_client ? c->keys.control_s2c : c->keys.control_c2s;
    const uint8_t *nb = c->is_client ? c->keys.nonce_s2c : c->keys.nonce_c2s;
    size_t z = 0;
    int rc = ct_aead_decrypt(c->cipher, k, nb, h->sequence, head, sizeof head, box, h->payload_len,
                             p, &z);
    free(box);
    if (rc)
        return -1;
    c->rx_seq = h->sequence;
    c->last_rx_ms = ct_monotonic_ms();
    *n = z;
    return 0;
}
static size_t transcript(uint8_t *out, size_t cap, const uint8_t *ch, size_t cn,
                         const uint8_t sr[32], const uint8_t ep[32], uint8_t cipher,
                         const uint8_t client_identity[32], const uint8_t server_identity[32]) {
    const char tag[] = "ctunnel-handshake-v3";
    size_t n = sizeof(tag) - 1 + 5 + cn + 65 + 64;
    if (n > cap)
        return 0;
    size_t o = 0;
    memcpy(out + o, tag, sizeof(tag) - 1);
    o += sizeof(tag) - 1;
    ct_put_u32(out + o, CT_MAGIC);
    o += 4;
    out[o++] = CT_PROTOCOL_VERSION;
    memcpy(out + o, ch, cn);
    o += cn;
    memcpy(out + o, sr, 32);
    o += 32;
    memcpy(out + o, ep, 32);
    o += 32;
    out[o++] = cipher;
    memcpy(out + o, client_identity, 32);
    o += 32;
    memcpy(out + o, server_identity, 32);
    o += 32;
    return o;
}
static size_t signature_message(uint8_t out[64], const char *role, const uint8_t *tr, size_t tn) {
    size_t role_length = strlen(role);
    if (role_length + 32 > 64)
        return 0;
    memcpy(out, role, role_length);
    ct_crypto_hash(out + role_length, tr, tn);
    return role_length + 32;
}
static ct_cipher choose(unsigned offered, unsigned allowed, ct_cipher preferred) {
    ct_cipher list[2] = {preferred, CT_CIPHER_CHACHA};
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) {
        ct_cipher c = list[i];
        if ((offered & (1u << c)) && (allowed & (1u << c)) && ct_cipher_available(c))
            return c;
    }
    return CT_CIPHER_NONE;
}
int ct_handshake_client(ct_socket fd, const ct_config *cfg, ct_control *c) {
    uint8_t idsk[64], xpk[32], xsk[32], cr[32], ch[CT_CONTROL_BUFFER_SIZE],
        reply[CT_CONTROL_BUFFER_SIZE], tr[CT_CONTROL_BUFFER_SIZE * 2], shared[32], serverpk[32],
        signed_message[64], client_identity[32], server_identity[32];
    size_t o = 0, rn, tn, signed_length;
    ct_frame_header h;
    memset(c, 0, sizeof *c);
    memset(idsk, 0, sizeof idsk);
    memset(xsk, 0, sizeof xsk);
    memset(shared, 0, sizeof shared);
    memset(signed_message, 0, sizeof signed_message);
    if (ct_load_private_key(cfg->identity_private_key, idsk) ||
        ct_load_public_key(cfg->server_public_key, serverpk) || ct_x_keypair(xpk, xsk) ||
        ct_platform_random(cr, 32))
        goto bad;
    if (ct_pack_string(ch, sizeof ch, &o, cfg->client_id, CT_MAX_CLIENT_ID))
        goto bad;
    memcpy(ch + o, cr, 32);
    o += 32;
    memcpy(ch + o, xpk, 32);
    o += 32;
    ct_put_u32(ch + o, cfg->cipher_mask);
    o += 4;
    if (ct_plain_send(fd, CT_MSG_CLIENT_HELLO, 0, 0, ch, o, cfg->handshake_timeout * 1000) ||
        ct_plain_recv(fd, &h, reply, sizeof reply, &rn, cfg->handshake_timeout * 1000) ||
        h.type != CT_MSG_SERVER_HELLO || h.session_id || h.stream_id || h.sequence || rn != 129)
        goto bad;
    uint8_t *sr = reply, *sep = reply + 32;
    ct_cipher selected = (ct_cipher)reply[64];
    if (selected != CT_CIPHER_CHACHA || !(cfg->cipher_mask & (1u << selected)) ||
        !ct_cipher_available(selected))
        goto bad;
    ct_crypto_hash(client_identity, idsk + 32, 32);
    ct_crypto_hash(server_identity, serverpk, 32);
    tn = transcript(tr, sizeof tr, ch, o, sr, sep, (uint8_t)selected, client_identity,
                    server_identity);
    signed_length = signature_message(signed_message, "ctunnel server handshake v3", tr, tn);
    if (!tn || !signed_length || ct_ed_verify(reply + 65, signed_message, signed_length, serverpk))
        goto bad;
    uint8_t sig[64];
    signed_length = signature_message(signed_message, "ctunnel client handshake v3", tr, tn);
    if (!signed_length || ct_ed_sign(sig, signed_message, signed_length, idsk) ||
        ct_plain_send(fd, CT_MSG_CLIENT_AUTH, 0, 0, sig, 64, cfg->handshake_timeout * 1000) ||
        ct_x_shared(shared, xsk, sep))
        goto bad;
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->cipher = selected;
    c->is_client = 1;
    if (strlen(cfg->client_id) > CT_MAX_CLIENT_ID)
        goto bad;
    memcpy(c->client_id, cfg->client_id, strlen(cfg->client_id) + 1);
    if (ct_derive_session(shared, tr, tn, &c->keys))
        goto bad;
    if (ct_control_recv(c, &h, reply, sizeof reply, &rn, cfg->handshake_timeout * 1000) ||
        h.type != CT_MSG_AUTH_OK)
        goto bad;
    c->last_rx_ms = c->last_tx_ms = ct_monotonic_ms();
    ct_secure_zero(idsk, sizeof idsk);
    ct_secure_zero(xsk, sizeof xsk);
    ct_secure_zero(shared, sizeof shared);
    ct_secure_zero(signed_message, sizeof signed_message);
    return 0;
bad:
    ct_secure_zero(&c->keys, sizeof c->keys);
    ct_secure_zero(idsk, sizeof idsk);
    ct_secure_zero(xsk, sizeof xsk);
    ct_secure_zero(shared, sizeof shared);
    ct_secure_zero(signed_message, sizeof signed_message);
    return -1;
}
void ct_handshake_server_abort(ct_server_handshake *handshake) {
    ct_secure_zero(handshake, sizeof *handshake);
}

int ct_handshake_server_begin(ct_socket fd, const ct_config *cfg, ct_server_handshake *handshake) {
    uint8_t ch[CT_CONTROL_BUFFER_SIZE], reply[CT_CONTROL_BUFFER_SIZE], xpk[32], sr[32], idsk[64],
        signed_message[64], client_identity[32], server_identity[32];
    size_t n, o = 0, tn, signed_length;
    ct_frame_header h;
    char id[CT_MAX_CLIENT_ID + 1];
    memset(handshake, 0, sizeof *handshake);
    memset(idsk, 0, sizeof idsk);
    memset(signed_message, 0, sizeof signed_message);
    if (ct_plain_recv(fd, &h, ch, sizeof ch, &n, cfg->handshake_timeout * 1000) ||
        h.type != CT_MSG_CLIENT_HELLO || h.session_id || h.stream_id || h.sequence ||
        ct_unpack_string(ch, n, &o, id, sizeof id) || n - o != 68)
        goto bad;
    const ct_authorized_client *a = ct_authorized_find(cfg, id);
    if (!a || ct_load_public_key(a->public_key, handshake->client_public) ||
        ct_load_private_key(cfg->identity_private_key, idsk))
        goto bad;
    uint8_t *cep = ch + o + 32;
    (void)cep;
    unsigned offered = ct_get_u32(ch + o + 64);
    ct_cipher selected = choose(offered, cfg->cipher_mask, cfg->preferred_cipher);
    if (!selected || ct_x_keypair(xpk, handshake->ephemeral_private) || ct_platform_random(sr, 32))
        goto bad;
    ct_crypto_hash(client_identity, handshake->client_public, 32);
    ct_crypto_hash(server_identity, idsk + 32, 32);
    tn = transcript(handshake->transcript, sizeof handshake->transcript, ch, n, sr, xpk,
                    (uint8_t)selected, client_identity, server_identity);
    if (!tn)
        goto bad;
    memcpy(reply, sr, 32);
    memcpy(reply + 32, xpk, 32);
    reply[64] = (uint8_t)selected;
    signed_length =
        signature_message(signed_message, "ctunnel server handshake v3", handshake->transcript, tn);
    if (!signed_length || ct_ed_sign(reply + 65, signed_message, signed_length, idsk) ||
        ct_plain_send(fd, CT_MSG_SERVER_HELLO, 0, 0, reply, 129, cfg->handshake_timeout * 1000))
        goto bad;
    handshake->transcript_length = tn;
    handshake->cipher = selected;
    handshake->authorized = a;
    memcpy(handshake->client_id, id, strlen(id) + 1);
    ct_secure_zero(idsk, sizeof idsk);
    ct_secure_zero(signed_message, sizeof signed_message);
    return 0;
bad:
    ct_secure_zero(idsk, sizeof idsk);
    ct_secure_zero(signed_message, sizeof signed_message);
    ct_handshake_server_abort(handshake);
    return -1;
}

int ct_handshake_server_finish(ct_socket fd, const ct_config *cfg, ct_server_handshake *handshake,
                               ct_control *c, const ct_authorized_client **auth) {
    uint8_t reply[CT_CONTROL_BUFFER_SIZE], shared[32] = {0}, signed_message[64] = {0};
    size_t n, signed_length;
    ct_frame_header h;
    if (!handshake->authorized ||
        ct_plain_recv(fd, &h, reply, sizeof reply, &n, cfg->handshake_timeout * 1000) ||
        h.type != CT_MSG_CLIENT_AUTH || h.session_id || h.stream_id || h.sequence || n != 64)
        goto bad;
    signed_length = signature_message(signed_message, "ctunnel client handshake v3",
                                      handshake->transcript, handshake->transcript_length);
    /* CLIENT_HELLO fixes the peer ephemeral at: domain + magic/version + id-len/id + random. */
    const size_t prefix_length = sizeof("ctunnel-handshake-v3") - 1 + 5;
    size_t id_length = strlen(handshake->client_id);
    size_t peer_offset = prefix_length + 2 + id_length + 32;
    if (!signed_length ||
        ct_ed_verify(reply, signed_message, signed_length, handshake->client_public) ||
        peer_offset > handshake->transcript_length ||
        handshake->transcript_length - peer_offset < CT_X_PUBLIC ||
        ct_x_shared(shared, handshake->ephemeral_private, handshake->transcript + peer_offset))
        goto bad;
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->cipher = handshake->cipher;
    c->is_client = 0;
    memcpy(c->client_id, handshake->client_id, strlen(handshake->client_id) + 1);
    if (ct_derive_session(shared, handshake->transcript, handshake->transcript_length, &c->keys))
        goto bad;
    if (ct_control_send(c, CT_MSG_AUTH_OK, 0, NULL, 0, cfg->handshake_timeout * 1000))
        goto bad;
    c->last_rx_ms = c->last_tx_ms = ct_monotonic_ms();
    *auth = handshake->authorized;
    ct_secure_zero(shared, sizeof shared);
    ct_secure_zero(signed_message, sizeof signed_message);
    ct_handshake_server_abort(handshake);
    return 0;
bad:
    ct_secure_zero(&c->keys, sizeof c->keys);
    ct_secure_zero(shared, sizeof shared);
    ct_secure_zero(signed_message, sizeof signed_message);
    ct_handshake_server_abort(handshake);
    return -1;
}

int ct_handshake_server(ct_socket fd, const ct_config *cfg, ct_control *c,
                        const ct_authorized_client **auth) {
    ct_server_handshake handshake;
    if (ct_handshake_server_begin(fd, cfg, &handshake))
        return -1;
    return ct_handshake_server_finish(fd, cfg, &handshake, c, auth);
}
static void workmac(uint8_t out[32], const ct_control *c, const uint8_t work_id[32]) {
    uint8_t b[160];
    const char label[] = "ctunnel-v3/work-bind/client-to-server";
    size_t client_length = strlen(c->client_id), offset = 0;
    memcpy(b + offset, label, sizeof label - 1);
    offset += sizeof label - 1;
    ct_put_u64(b + offset, c->keys.session_id);
    offset += 8;
    ct_put_u16(b + offset, (uint16_t)client_length);
    offset += 2;
    memcpy(b + offset, c->client_id, client_length);
    offset += client_length;
    b[offset++] = CT_MSG_WORK_CONNECTION_BIND;
    memcpy(b + offset, work_id, 32);
    offset += 32;
    ct_crypto_mac(out, c->keys.work_auth_key, b, offset);
    ct_secure_zero(b, sizeof b);
}
int ct_work_connect(const ct_config *cfg, ct_control *c, ct_work *out) {
    ct_socket f = ct_net_connect(cfg->server_addr, cfg->server_port, cfg->connect_timeout);
    if (f == CT_INVALID_SOCKET)
        return -1;
    uint8_t p[64], mac[32];
    if (c->work_tx_seq == UINT64_MAX) {
        ct_socket_close(f);
        return -1;
    }
    ct_put_u64(p, ++c->work_tx_seq);
    if (ct_platform_random(p + 8, 24)) {
        ct_socket_close(f);
        return -1;
    }
    workmac(mac, c, p);
    memcpy(p + 32, mac, 32);
    if (ct_plain_send(f, CT_MSG_WORK_CONNECTION_BIND, c->keys.session_id, 0, p, 64,
                      cfg->connect_timeout * 1000)) {
        ct_socket_close(f);
        return -1;
    }
    out->fd = f;
    memcpy(out->id, p, 32);
    return 0;
}
static int accept_work_sequence(ct_control *c, uint64_t sequence) {
    if (!sequence)
        return -1;
    if (sequence > c->work_rx_high) {
        uint64_t advance = sequence - c->work_rx_high;
        c->work_rx_bitmap = advance >= 64 ? 1 : (c->work_rx_bitmap << advance) | 1;
        c->work_rx_high = sequence;
        return 0;
    }
    uint64_t age = c->work_rx_high - sequence;
    if (age >= 64)
        return -1;
    uint64_t bit = UINT64_C(1) << age;
    if (c->work_rx_bitmap & bit)
        return -1;
    c->work_rx_bitmap |= bit;
    return 0;
}
int ct_work_accept_bind(ct_socket f, ct_control *c, ct_work *out) {
    ct_frame_header h;
    uint8_t p[64], m[32];
    size_t n;
    if (ct_plain_recv(f, &h, p, sizeof p, &n, 5000) || h.type != CT_MSG_WORK_CONNECTION_BIND ||
        h.session_id != c->keys.session_id || h.stream_id || h.sequence || n != 64)
        return -1;
    workmac(m, c, p);
    if (!ct_crypto_equal(m, p + 32, 32))
        return -1;
    if (accept_work_sequence(c, ct_get_u64(p)))
        return -1;
    out->fd = f;
    memcpy(out->id, p, 32);
    return 0;
}
static int streammac(uint8_t out[32], const ct_control *c, uint8_t message_type,
                     uint8_t sender_role, const uint8_t work_id[32], const char *service_id,
                     ct_enc_mode mode, uint64_t stream_id, const uint8_t random[32], uint8_t ok) {
    uint8_t authenticated[256];
    const char label[] = "ctunnel-v3/stream-binding";
    size_t client_length = strlen(c->client_id), service_length = strlen(service_id), offset = 0;
    if (client_length > CT_MAX_CLIENT_ID || service_length > CT_MAX_SERVICE_ID)
        return -1;
    memcpy(authenticated + offset, label, sizeof label - 1);
    offset += sizeof label - 1;
    ct_put_u64(authenticated + offset, c->keys.session_id);
    offset += 8;
    ct_put_u16(authenticated + offset, (uint16_t)client_length);
    offset += 2;
    memcpy(authenticated + offset, c->client_id, client_length);
    offset += client_length;
    memcpy(authenticated + offset, work_id, 32);
    offset += 32;
    ct_put_u16(authenticated + offset, (uint16_t)service_length);
    offset += 2;
    memcpy(authenticated + offset, service_id, service_length);
    offset += service_length;
    ct_put_u64(authenticated + offset, stream_id);
    offset += 8;
    memcpy(authenticated + offset, random, 32);
    offset += 32;
    authenticated[offset++] = (uint8_t)mode;
    authenticated[offset++] = message_type;
    authenticated[offset++] = sender_role;
    authenticated[offset++] = ok;
    ct_crypto_mac(out, c->keys.stream_auth_key, authenticated, offset);
    ct_secure_zero(authenticated, sizeof authenticated);
    return 0;
}
int ct_start_stream_send(const ct_work *work, const ct_control *c, const char *id, uint64_t sid,
                         ct_enc_mode encm, uint8_t rnd[32]) {
    uint8_t p[CT_CONTROL_BUFFER_SIZE], mac[32];
    size_t o = 0;
    if (ct_pack_string(p, sizeof p, &o, id, CT_MAX_SERVICE_ID))
        return -1;
    p[o++] = (uint8_t)encm;
    memcpy(p + o, work->id, 32);
    o += 32;
    if (ct_platform_random(rnd, 32))
        return -1;
    memcpy(p + o, rnd, 32);
    o += 32;
    ct_put_u64(p + o, sid);
    o += 8;
    if (streammac(mac, c, CT_MSG_START_STREAM, 0, work->id, id, encm, sid, rnd, 1))
        return -1;
    memcpy(p + o, mac, 32);
    o += 32;
    return ct_plain_send(work->fd, CT_MSG_START_STREAM, c->keys.session_id, sid, p, o, 5000);
}
int ct_start_stream_recv(const ct_work *work, const ct_control *c, char *id, size_t cap,
                         uint64_t *sid, ct_enc_mode *encm, uint8_t rnd[32]) {
    ct_frame_header h;
    uint8_t p[CT_CONTROL_BUFFER_SIZE], m[32];
    size_t n, o = 0;
    if (ct_plain_recv(work->fd, &h, p, sizeof p, &n, 60000) || h.type != CT_MSG_START_STREAM ||
        h.session_id != c->keys.session_id || h.sequence || n < 107 ||
        ct_unpack_string(p, n, &o, id, cap) || n - o != 105)
        return -1;
    *encm = (ct_enc_mode)p[o++];
    if (*encm != CT_ENC_REQUIRED && *encm != CT_ENC_DISABLED)
        return -1;
    if (!ct_crypto_equal(p + o, work->id, 32))
        return -1;
    o += 32;
    memcpy(rnd, p + o, 32);
    o += 32;
    *sid = ct_get_u64(p + o);
    o += 8;
    if (*sid != h.stream_id)
        return -1;
    if (streammac(m, c, CT_MSG_START_STREAM, 0, work->id, id, *encm, *sid, rnd, 1))
        return -1;
    return ct_crypto_equal(m, p + o, 32) ? 0 : -1;
}
int ct_stream_ready_send(const ct_work *work, const ct_control *c, const char *service_id,
                         ct_enc_mode mode, uint64_t sid, const uint8_t rnd[32], int ok) {
    uint8_t p[CT_CONTROL_BUFFER_SIZE], m[32];
    size_t o = 0;
    uint8_t message_type = ok ? CT_MSG_STREAM_READY : CT_MSG_STREAM_FAILED;
    if (ct_pack_string(p, sizeof p, &o, service_id, CT_MAX_SERVICE_ID))
        return -1;
    p[o++] = (uint8_t)mode;
    memcpy(p + o, work->id, 32);
    o += 32;
    memcpy(p + o, rnd, 32);
    o += 32;
    ct_put_u64(p + o, sid);
    o += 8;
    p[o++] = (uint8_t)ok;
    if (streammac(m, c, message_type, 1, work->id, service_id, mode, sid, rnd, (uint8_t)ok))
        return -1;
    memcpy(p + o, m, 32);
    o += 32;
    return ct_plain_send(work->fd, message_type, c->keys.session_id, sid, p, o, 5000);
}
int ct_stream_ready_recv(const ct_work *work, const ct_control *c, const char *service_id,
                         ct_enc_mode mode, uint64_t sid, const uint8_t rnd[32], int *ok) {
    ct_frame_header h;
    uint8_t p[CT_CONTROL_BUFFER_SIZE], m[32];
    char received_service[CT_MAX_SERVICE_ID + 1];
    size_t n, o = 0;
    if (ct_plain_recv(work->fd, &h, p, sizeof p, &n, 5000) || h.session_id != c->keys.session_id ||
        h.stream_id != sid || h.sequence ||
        (h.type != CT_MSG_STREAM_READY && h.type != CT_MSG_STREAM_FAILED) ||
        ct_unpack_string(p, n, &o, received_service, sizeof received_service) || n - o != 106 ||
        strcmp(received_service, service_id) || p[o++] != (uint8_t)mode ||
        !ct_crypto_equal(p + o, work->id, 32))
        return -1;
    o += 32;
    if (memcmp(p + o, rnd, 32))
        return -1;
    o += 32;
    if (ct_get_u64(p + o) != sid)
        return -1;
    o += 8;
    uint8_t received_ok = p[o++];
    if (received_ok > 1 ||
        streammac(m, c, h.type, 1, work->id, service_id, mode, sid, rnd, received_ok) ||
        !ct_crypto_equal(m, p + o, 32))
        return -1;
    *ok = received_ok && h.type == CT_MSG_STREAM_READY;
    return 0;
}
