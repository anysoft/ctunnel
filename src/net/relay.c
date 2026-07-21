#include "net/relay.h"
#include "protocol/protocol.h"
#include "util/metrics.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif
#if CONFIG_STREAM_BUFFER_SIZE < 32768
#define CHUNK (CONFIG_STREAM_BUFFER_SIZE / 2U)
#else
#define CHUNK 16384U
#endif
static int append(ct_ring *r, const void *p, size_t n) {
    return ct_ring_write(r, p, n) == n ? 0 : -1;
}
static size_t buffered_bytes(const ct_relay *r) {
    size_t n = r->to_direct.len + r->to_work.len;
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    n += r->work_input.len;
#endif
    return n;
}
static void sync_buffer_metric(ct_relay *r) {
    size_t now = buffered_bytes(r);
    if (now >= r->accounted_buffer_bytes)
        ct_metric_add(CT_METRIC_BUFFER_BYTES_CURRENT, now - r->accounted_buffer_bytes);
    else
        ct_metric_sub(CT_METRIC_BUFFER_BYTES_CURRENT, r->accounted_buffer_bytes - now);
    r->accounted_buffer_bytes = now;
}
static void metric_bytes(const ct_relay *r, ct_socket source, size_t n) {
    int direct_to_work = source == r->direct;
    int c2s = r->is_client ? direct_to_work : !direct_to_work;
    ct_metric_add(c2s ? CT_METRIC_BYTES_C2S_TOTAL : CT_METRIC_BYTES_S2C_TOTAL, n);
}
int ct_relay_init(ct_relay *r, ct_socket direct, ct_socket work, int client, ct_enc_mode mode,
                  ct_cipher cipher, const uint8_t master[32], uint64_t sid, const uint8_t rnd[32],
                  const uint8_t work_id[32], const char *service_id,
                  const ct_stream_metadata *metadata) {
    memset(r, 0, sizeof *r);
    r->direct = direct;
    r->work = work;
    r->is_client = client;
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    r->encrypted = mode == CT_ENC_REQUIRED;
    r->cipher = cipher;
    if (ct_ring_init(&r->to_direct, CT_IO_BUFFER_SIZE) ||
        ct_ring_init(&r->to_work, CT_IO_BUFFER_SIZE) ||
        ct_ring_init(&r->work_input, CT_IO_BUFFER_SIZE)) {
        ct_relay_close(r);
        return -1;
    }
    if (r->encrypted) {
        int senddir = client ? 0 : 1, recvdir = client ? 1 : 0;
        if (ct_derive_data(master, sid, rnd, work_id, service_id, mode, senddir, r->send_key,
                           r->send_nonce) ||
            ct_derive_data(master, sid, rnd, work_id, service_id, mode, recvdir, r->recv_key,
                           r->recv_nonce)) {
            ct_relay_close(r);
            return -1;
        }
    }
#else
    (void)client;
    (void)cipher;
    (void)master;
    (void)sid;
    (void)rnd;
    (void)work_id;
    (void)service_id;
    if (mode != CT_ENC_DISABLED || ct_ring_init(&r->to_direct, CT_IO_BUFFER_SIZE) ||
        ct_ring_init(&r->to_work, CT_IO_BUFFER_SIZE)) {
        ct_relay_close(r);
        return -1;
    }
#endif
#ifndef CONFIG_FEATURE_PROXY_PROTOCOL
    (void)metadata;
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL
    if (client && metadata && metadata->proxy_protocol != CT_PROXY_PROTOCOL_OFF) {
        uint8_t header[CT_PROXY_PROTOCOL_MAX_HEADER];
        size_t header_length = 0;
        if (ct_proxy_protocol_build(header, sizeof header, &header_length, metadata) ||
            append(&r->to_direct, header, header_length)) {
            ct_metric_inc(CT_METRIC_PROXY_PROTOCOL_HEADER_FAILURES_TOTAL);
            ct_relay_close(r);
            return -1;
        }
        r->proxy_header_pending = 1;
        r->proxy_header_remaining = header_length;
        if (metadata->proxy_protocol == CT_PROXY_PROTOCOL_V1)
            ct_metric_inc(CT_METRIC_PROXY_PROTOCOL_V1_CONNECTIONS_TOTAL);
        else if (metadata->proxy_protocol == CT_PROXY_PROTOCOL_V2)
            ct_metric_inc(CT_METRIC_PROXY_PROTOCOL_V2_CONNECTIONS_TOTAL);
    } else
#endif
    {
        if (client)
            ct_metric_inc(CT_METRIC_PROXY_PROTOCOL_DISABLED_CONNECTIONS_TOTAL);
    }
    ct_metric_inc(CT_METRIC_STREAMS_OPENED_TOTAL);
    ct_metric_inc(CT_METRIC_ACTIVE_STREAMS);
    ct_metric_inc(CT_METRIC_WORK_ACTIVE);
    r->stream_accounted = 1;
    return 0;
}
void ct_relay_close(ct_relay *r) {
    if (!r->closed && r->stream_accounted) {
        sync_buffer_metric(r);
        size_t accounted = r->accounted_buffer_bytes;
        ct_metric_sub(CT_METRIC_BUFFER_BYTES_CURRENT, accounted);
        r->accounted_buffer_bytes = 0;
        ct_metric_inc(CT_METRIC_STREAMS_CLOSED_TOTAL);
        ct_metric_dec(CT_METRIC_ACTIVE_STREAMS);
        ct_metric_dec(CT_METRIC_WORK_ACTIVE);
        r->stream_accounted = 0;
    }
    if (!r->closed) {
        ct_socket_close(r->direct);
        ct_socket_close(r->work);
    }
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    ct_secure_zero(r->send_key, 32);
    ct_secure_zero(r->recv_key, 32);
    ct_ring_free(&r->work_input);
#endif
    ct_ring_free(&r->to_direct);
    ct_ring_free(&r->to_work);
    r->closed = 1;
}
int ct_relay_events(const ct_relay *r, ct_socket fd) {
    if (r->closed)
        return 0;
    int ev = 0;
    if (fd == r->direct) {
        if (!r->direct_eof && r->to_work.len < r->to_work.cap - CHUNK - 32)
            ev |= 1;
        if (r->to_direct.len)
            ev |= 2;
    } else if (fd == r->work) {
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
        if (!r->work_eof && r->work_input.len < r->work_input.cap - CHUNK - 32)
#else
        if (!r->work_eof && r->to_direct.len < r->to_direct.cap - CHUNK)
#endif
            ev |= 1;
        if (r->to_work.len)
            ev |= 2;
    }
    return ev;
}
static int flush(ct_socket f, ct_ring *r) {
    const uint8_t *p;
    size_t n = ct_ring_peek(r, &p);
    if (!n)
        return 0;
#ifdef _WIN32
    int z = send(f, (const char *)p, (int)n, 0);
#else
    int z = (int)send(f, p, n, 0);
#endif
    if (z > 0) {
        ct_ring_consume(r, (size_t)z);
        return 0;
    }
    return z < 0 && ct_socket_would_block() ? 0 : -1;
}
static int flush_relay(ct_relay *relay, ct_socket fd, ct_ring *ring) {
    size_t before = ring->len;
    int rc = flush(fd, ring);
    if (rc && relay->proxy_header_pending && fd == relay->direct)
        ct_metric_inc(CT_METRIC_PROXY_PROTOCOL_LOCAL_WRITE_FAILURES_TOTAL);
    if (!rc && relay->proxy_header_pending && fd == relay->direct) {
        size_t consumed = before >= ring->len ? before - ring->len : 0;
        if (consumed >= relay->proxy_header_remaining) {
            relay->proxy_header_remaining = 0;
            relay->proxy_header_pending = 0;
        } else {
            relay->proxy_header_remaining -= consumed;
        }
    }
    return rc;
}
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
static int encrypt_chunk(ct_relay *r, const uint8_t *p, size_t n) {
    uint8_t record[12 + CHUNK + CT_AEAD_TAG], *box = record + 12;
    size_t z = 0;
    if (r->send_seq == UINT64_MAX)
        return -1;
    uint64_t seq = ++r->send_seq;
    ct_put_u32(record, (uint32_t)(n + CT_AEAD_TAG));
    ct_put_u64(record + 4, seq);
    if (ct_aead_encrypt(r->cipher, r->send_key, r->send_nonce, seq, record, 12, p, n, box, &z)) {
        ct_metric_inc(CT_METRIC_AEAD_FAILURES_TOTAL);
        return -1;
    }
    return append(&r->to_work, record, 12 + z);
}
/* Peek a possibly wrapped header and consume only complete authenticated records. */
static int decrypt_ready(ct_relay *r) {
    while (r->work_input.len >= 12) {
        uint8_t h[12], box[CHUNK + CT_AEAD_TAG], plain[CHUNK];
        ct_ring rr = r->work_input;
        ct_ring_read(&rr, h, 12);
        uint32_t n;
        uint64_t seq;
        if (ct_data_record_header_decode(h, CHUNK, r->recv_seq, &n, &seq)) {
            ct_metric_inc(CT_METRIC_AEAD_FAILURES_TOTAL);
            return -1;
        }
        if (r->work_input.len < 12 + n)
            return 0;
        if (r->to_direct.cap - r->to_direct.len < n - CT_AEAD_TAG)
            return 0;
        ct_ring_read(&r->work_input, h, 12);
        ct_ring_read(&r->work_input, box, n);
        size_t z = 0;
        if (ct_aead_decrypt(r->cipher, r->recv_key, r->recv_nonce, seq, h, 12, box, n, plain, &z)) {
            ct_metric_inc(CT_METRIC_AEAD_FAILURES_TOTAL);
            return -1;
        }
        if (append(&r->to_direct, plain, z))
            return -1;
        r->recv_seq = seq;
    }
    return 0;
}
#endif
static int read_direct(ct_relay *r) {
    uint8_t b[CHUNK];
    int n = (int)recv(r->direct, (char *)b, sizeof b, 0);
    if (n > 0) {
        metric_bytes(r, r->direct, (size_t)n);
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
        return r->encrypted ? encrypt_chunk(r, b, (size_t)n) : append(&r->to_work, b, (size_t)n);
#else
        return append(&r->to_work, b, (size_t)n);
#endif
    }
    if (n == 0) {
        r->direct_eof = 1;
        return 0;
    }
    return ct_socket_would_block() ? 0 : -1;
}
static int read_work(ct_relay *r) {
    uint8_t b[CHUNK + 32];
    int n = (int)recv(r->work, (char *)b, sizeof b, 0);
    if (n > 0) {
        metric_bytes(r, r->work, (size_t)n);
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
        if (r->encrypted) {
            if (append(&r->work_input, b, (size_t)n))
                return -1;
            return decrypt_ready(r);
        }
#endif
        return append(&r->to_direct, b, (size_t)n);
    }
    if (n == 0) {
        r->work_eof = 1;
        return 0;
    }
    return ct_socket_would_block() ? 0 : -1;
}
int ct_relay_process(ct_relay *r, ct_socket f, int ev) {
    if (r->closed)
        return -1;
    if ((ev & 2) && flush_relay(r, f, f == r->direct ? &r->to_direct : &r->to_work))
        goto bad;
    sync_buffer_metric(r);
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    /*
     * Encrypted work input may contain complete records that could not be
     * decrypted earlier because the direct-side output ring was full. A later
     * direct-side flush creates room, but no new work-side read event is
     * guaranteed to arrive. Drain again here to avoid large responses stalling
     * after only a prefix reaches the browser/client.
     */
    if (f == r->direct && r->encrypted && r->work_input.len && decrypt_ready(r))
        goto bad;
    sync_buffer_metric(r);
#endif
    if (ev & 1) {
        if ((f == r->direct ? read_direct(r) : read_work(r)))
            goto bad;
    }
    sync_buffer_metric(r);
    if (r->direct_eof && !r->to_work.len) {
#ifdef _WIN32
        shutdown(r->work, SD_SEND);
#else
        shutdown(r->work, SHUT_WR);
#endif
    }
    if (r->work_eof && !r->to_direct.len) {
#ifdef _WIN32
        shutdown(r->direct, SD_SEND);
#else
        shutdown(r->direct, SHUT_WR);
#endif
    }
    if (r->direct_eof && r->work_eof && !r->to_direct.len && !r->to_work.len) {
        ct_relay_close(r);
        return -1;
    }
    return 0;
bad:
    ct_metric_inc(CT_METRIC_STREAMS_FAILED_TOTAL);
    ct_relay_close(r);
    return -1;
}
