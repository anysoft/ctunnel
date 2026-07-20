#include "app/runtime.h"
#include "ctunnel.h"
#include "net/net.h"
#include "net/relay.h"
#include "platform/event.h"
#include "platform/platform.h"
#include "protocol/protocol.h"
#include "util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#define MAX_WORK CONFIG_MAX_STREAMS
#define MAX_RELAYS CONFIG_MAX_STREAMS

enum { REF_CONTROL, REF_WORK, REF_RELAY_DIRECT, REF_RELAY_WORK };
typedef struct {
    int kind;
    void *peer;
    void *ptr;
} ref;

typedef struct {
    ct_work connection;
    uint64_t read_retry_after_ms;
} idle_work;
static const ct_service_config *client_svc(const ct_config *c, const char *id) {
    for (size_t i = 0; i < c->service_count; i++)
        if (!strcmp(c->services[i].id, id))
            return &c->services[i];
    return NULL;
}
static int add_work(const ct_config *cfg, ct_control *c, idle_work *w, size_t *n) {
    if (*n >= MAX_WORK)
        return -1;
    ct_work connection;
    if (ct_work_connect(cfg, c, &connection))
        return -1;
    w[*n].connection = connection;
    w[*n].read_retry_after_ms = 0;
    (*n)++;
    return 0;
}
static idle_work *find_work(idle_work *work, size_t work_count, ct_socket fd, size_t *index) {
    for (size_t i = 0; i < work_count; i++)
        if (work[i].connection.fd == fd) {
            if (index)
                *index = i;
            return &work[i];
        }
    return NULL;
}
static int client_register(const ct_config *cfg, ct_control *c) {
    for (size_t i = 0; i < cfg->service_count; i++) {
        const ct_service_config *s = &cfg->services[i];
        uint8_t b[CT_CONTROL_BUFFER_SIZE], reply[CT_CONTROL_BUFFER_SIZE];
        size_t o = 0, n;
        ct_frame_header h;
        if (ct_pack_string(b, sizeof b, &o, s->id, CT_MAX_SERVICE_ID) ||
            ct_pack_string(b, sizeof b, &o, s->remote_addr, CT_MAX_ADDR))
            return -1;
        ct_put_u16(b + o, s->remote_port);
        o += 2;
        b[o++] = (uint8_t)s->type;
        b[o++] = (uint8_t)s->encryption;
        if (ct_control_send(c, CT_MSG_REGISTER_SERVICE, 0, b, o, 5000))
            return -1;
        for (;;) {
            if (ct_control_recv(c, &h, reply, sizeof reply, &n, 5000))
                return -1;
            if (h.type == CT_MSG_PING) {
                if (ct_control_send(c, CT_MSG_PONG, 0, reply, n, 5000))
                    return -1;
                continue;
            }
            if (h.type == CT_MSG_REQUEST_WORK_CONNECTION)
                continue;
            if (h.type != CT_MSG_REGISTER_OK)
                return -1;
            char registered_id[CT_MAX_SERVICE_ID + 1];
            size_t reply_offset = 0;
            if (ct_unpack_string(reply, n, &reply_offset, registered_id, sizeof registered_id) ||
                reply_offset != n || strcmp(registered_id, s->id))
                return -1;
            break;
        }
        CT_LOGI("client", "service_id=%s registered remote=%s:%u", s->id, s->remote_addr,
                s->remote_port);
    }
    return 0;
}
static int client_session(const ct_config *cfg) {
    idle_work *work = NULL;
    ct_relay *relays = NULL;
    ref *refs = NULL;
    ct_event *events = NULL;
    size_t wn = 0;
    ct_socket f = ct_net_connect(cfg->server_addr, cfg->server_port, cfg->connect_timeout);
    if (f == CT_INVALID_SOCKET)
        return -1;
    ct_control ctl;
    if (ct_handshake_client(f, cfg, &ctl)) {
        ct_socket_close(f);
        return -1;
    }
    CT_LOGI("client", "authenticated server=%s:%u cipher=%s", cfg->server_addr, cfg->server_port,
            ctl.cipher == CT_CIPHER_CHACHA ? "xchacha20-poly1305" : "unsupported");
    if (client_register(cfg, &ctl)) {
        ct_socket_close(f);
        return -1;
    }
    ct_log_status("client", "started client_id=%s server=%s:%u services=%zu log_file=%s",
                  cfg->client_id, cfg->server_addr, cfg->server_port, cfg->service_count,
                  cfg->log_file[0] ? cfg->log_file : "stderr");
    const size_t event_capacity = 1u + MAX_WORK + 2u * MAX_RELAYS;
    work = calloc(MAX_WORK, sizeof *work);
    relays = calloc(MAX_RELAYS, sizeof *relays);
    refs = calloc(event_capacity, sizeof *refs);
    events = calloc(event_capacity, sizeof *events);
    if (!work || !relays || !refs || !events) {
        free(work);
        free(relays);
        free(refs);
        free(events);
        ct_socket_close(ctl.fd);
        ct_secure_zero(&ctl.keys, sizeof ctl.keys);
        return -2;
    }
    for (size_t i = 0; i < MAX_RELAYS; i++) {
        relays[i].closed = 1;
        relays[i].direct = CT_INVALID_SOCKET;
    }
#ifdef CONFIG_FEATURE_WORK_POOL
    for (int i = 0; i < cfg->pool_count; i++)
        if (add_work(cfg, &ctl, work, &wn))
            break;
#endif
    while (!ct_runtime_should_stop()) {
        ct_event_loop *l = event_loop_create(event_capacity);
        if (!l)
            break;
        size_t rn = 0;
        uint64_t loop_now = ct_monotonic_ms();
        if (loop_now >= ctl.read_retry_after_ms) {
            refs[rn] = (ref){REF_CONTROL, NULL, NULL};
            event_loop_add(l, ctl.fd, CT_EV_READ, &refs[rn++]);
        }
        for (size_t i = 0; i < wn; i++) {
            if (loop_now < work[i].read_retry_after_ms)
                continue;
            refs[rn] = (ref){REF_WORK, NULL, (void *)(uintptr_t)work[i].connection.fd};
            event_loop_add(l, work[i].connection.fd, CT_EV_READ, &refs[rn++]);
        }
        for (size_t i = 0; i < MAX_RELAYS; i++)
            if (!relays[i].closed) {
                int ev = ct_relay_events(&relays[i], relays[i].direct);
                refs[rn] = (ref){REF_RELAY_DIRECT, NULL, &relays[i]};
                event_loop_add(l, relays[i].direct, ev, &refs[rn++]);
                ev = ct_relay_events(&relays[i], relays[i].work);
                refs[rn] = (ref){REF_RELAY_WORK, NULL, &relays[i]};
                event_loop_add(l, relays[i].work, ev, &refs[rn++]);
            }
        int ne = event_loop_wait(l, events, event_capacity, 50);
        event_loop_destroy(l);
        if (ne < 0)
            break;
        for (int ei = 0; ei < ne; ei++) {
            ref *r = (ref *)events[ei].user;
            if (r->kind == REF_CONTROL) {
                uint8_t b[CT_CONTROL_BUFFER_SIZE];
                size_t n;
                ct_frame_header h;
                int available = ct_frame_available(ctl.fd, CT_CONTROL_BUFFER_SIZE + CT_AEAD_TAG,
                                                   CT_FLAG_ENCRYPTED, &h);
                if (available < 0)
                    goto done;
                if (!available) {
                    ctl.read_retry_after_ms = ct_monotonic_ms() + 50;
                    continue;
                }
                ctl.read_retry_after_ms = 0;
                if (ct_control_recv(&ctl, &h, b, sizeof b, &n, 5000))
                    goto done;
                if (h.type == CT_MSG_PING) {
                    if (ct_control_send(&ctl, CT_MSG_PONG, 0, b, n, 5000))
                        goto done;
                } else if (h.type == CT_MSG_PONG) {
                    /* Expected response to the client's authenticated heartbeat. */
                } else if (h.type == CT_MSG_REQUEST_WORK_CONNECTION) {
                    (void)add_work(cfg, &ctl, work, &wn);
                } else if (h.type == CT_MSG_GOAWAY)
                    goto done;
                else
                    goto done;
            } else if (r->kind == REF_WORK) {
                ct_socket work_fd = (ct_socket)(uintptr_t)r->ptr;
                size_t wi;
                idle_work *iw = find_work(work, wn, work_fd, &wi);
                if (!iw)
                    continue;
                char id[CT_MAX_SERVICE_ID + 1];
                uint64_t sid;
                ct_enc_mode em;
                uint8_t rnd[32];
                ct_frame_header start_header;
                int available = ct_plain_frame_available(iw->connection.fd, CT_CONTROL_BUFFER_SIZE,
                                                         &start_header);
                if (available < 0) {
                    ct_socket_close(iw->connection.fd);
                    work[wi] = work[--wn];
                    continue;
                }
                if (!available) {
                    iw->read_retry_after_ms = ct_monotonic_ms() + 50;
                    continue;
                }
                ct_work wf = iw->connection;
                work[wi] = work[--wn];
                if (ct_start_stream_recv(&wf, &ctl, id, sizeof id, &sid, &em, rnd)) {
                    CT_LOGW("client", "rejected START_STREAM on authenticated work connection");
                    ct_socket_close(wf.fd);
                    continue;
                }
                const ct_service_config *s = client_svc(cfg, id);
                if (s && s->encryption != em)
                    s = NULL;
                ct_socket local =
                    s ? ct_net_connect(s->local_addr, s->local_port, cfg->connect_timeout)
                      : CT_INVALID_SOCKET;
                if (local == CT_INVALID_SOCKET) {
                    ct_stream_ready_send(&wf, &ctl, id, em, sid, rnd, 0);
                    ct_socket_close(wf.fd);
                } else {
                    ct_relay *rr = NULL;
                    for (size_t j = 0; j < MAX_RELAYS; j++)
                        if (relays[j].closed) {
                            rr = &relays[j];
                            break;
                        }
                    if (!rr || ct_stream_ready_send(&wf, &ctl, id, em, sid, rnd, 1) ||
                        ct_relay_init(rr, local, wf.fd, 1, em, ctl.cipher,
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
                                      ctl.keys.data_master,
#else
                                      NULL,
#endif
                                      sid, rnd, wf.id, id)) {
                        ct_socket_close(local);
                        ct_socket_close(wf.fd);
                    }
                }
#ifdef CONFIG_FEATURE_WORK_POOL
                while (wn < (size_t)cfg->pool_count && wn < MAX_WORK)
                    if (add_work(cfg, &ctl, work, &wn))
                        break;
#endif
            } else {
                ct_relay *rr = (ct_relay *)r->ptr;
                ct_socket fd = r->kind == REF_RELAY_DIRECT ? rr->direct : rr->work;
                (void)ct_relay_process(rr, fd, events[ei].events);
            }
        }
        uint64_t now = ct_monotonic_ms();
        if (now - ctl.last_rx_ms > (uint64_t)cfg->heartbeat_timeout * 1000u)
            break;
        if (now - ctl.last_tx_ms > (uint64_t)cfg->heartbeat_interval * 1000u) {
            uint8_t b[16];
            ct_put_u64(b, ctl.tx_seq + 1);
            ct_put_u64(b + 8, now);
            if (ct_control_send(&ctl, CT_MSG_PING, 0, b, sizeof b, 1000))
                break;
        }
    }
done:
    if (ct_runtime_should_stop())
        (void)ct_control_send(&ctl, CT_MSG_GOAWAY, 0, NULL, 0, 100);
    for (size_t i = 0; i < wn; i++)
        ct_socket_close(work[i].connection.fd);
    for (size_t i = 0; i < MAX_RELAYS; i++)
        if (!relays[i].closed)
            ct_relay_close(&relays[i]);
    ct_socket_close(ctl.fd);
    ct_secure_zero(&ctl.keys, sizeof ctl.keys);
    free(work);
    free(relays);
    free(refs);
    free(events);
    return ct_runtime_should_stop() ? 0 : -2;
}
int ct_run_client(const ct_config *cfg) {
    ct_runtime_init();
#ifndef CONFIG_FEATURE_CLIENT_RECONNECT
    return client_session(cfg) == -2 ? 1 : 0;
#else
    int delay = cfg->reconnect_initial_delay;
    while (!ct_runtime_should_stop()) {
        int result = client_session(cfg);
        if (result == 0 && ct_runtime_should_stop())
            break;
        if (result == -2)
            delay = cfg->reconnect_initial_delay;
        uint32_t r = 0;
        if (ct_platform_random((uint8_t *)&r, sizeof r))
            r = 0;
        int pct = cfg->reconnect_jitter_percent;
        int delta = (int)(r % (unsigned)(2 * pct + 1)) - pct;
        int ms = delay * 1000 * (100 + delta) / 100;
        CT_LOGW("client", "disconnected; reconnecting in %d ms", ms);
        ct_sleep_ms((unsigned)ms);
        if (delay < cfg->reconnect_max_delay) {
            delay *= 2;
            if (delay > cfg->reconnect_max_delay)
                delay = cfg->reconnect_max_delay;
        }
    }
    return 0;
#endif
}
