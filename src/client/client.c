#include "app/runtime.h"
#include "ctunnel.h"
#include "net/net.h"
#include "net/relay.h"
#include "platform/event.h"
#include "platform/platform.h"
#include "protocol/protocol.h"
#include "util/log.h"
#include "util/metrics.h"
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

enum { REF_CONTROL, REF_WORK, REF_UDP_LOCAL, REF_RELAY_DIRECT, REF_RELAY_WORK };
typedef struct {
    int kind;
    void *peer;
    void *ptr;
} ref;

typedef struct {
    ct_work connection;
    uint64_t read_retry_after_ms;
} idle_work;
#ifdef CONFIG_FEATURE_UDP
typedef struct {
    int active;
    uint64_t id, tx_seq, rx_high, rx_bitmap, last_ms;
    ct_udp_endpoint peer, local;
    const ct_service_config *svc;
    ct_socket fd;
} client_udp_session;
#endif
static const ct_service_config *client_svc(const ct_config *c, const char *id) {
    for (size_t i = 0; i < c->service_count; i++)
        if (!strcmp(c->services[i].id, id))
            return &c->services[i];
    return NULL;
}
#ifdef CONFIG_FEATURE_UDP
static int endpoint_equal(const ct_udp_endpoint *a, const ct_udp_endpoint *b) {
    size_t n = a->family == 4 ? 4U : a->family == 6 ? 16U : 0U;
    return n && a->family == b->family && a->port == b->port && !memcmp(a->addr, b->addr, n);
}

static int udp_replay_accept(uint64_t *high, uint64_t *bitmap, uint64_t seq) {
    if (!seq)
        return 0;
    if (seq > *high) {
        uint64_t shift = seq - *high;
        *bitmap = shift >= 64 ? 1 : ((*bitmap << shift) | 1);
        *high = seq;
        return 1;
    }
    uint64_t behind = *high - seq;
    if (behind >= CONFIG_UDP_REPLAY_WINDOW)
        return 0;
    uint64_t bit = UINT64_C(1) << behind;
    if (*bitmap & bit)
        return 0;
    *bitmap |= bit;
    return 1;
}

static client_udp_session *client_udp_find(client_udp_session *sessions, uint64_t id) {
    for (size_t i = 0; i < CONFIG_MAX_UDP_SESSIONS; i++)
        if (sessions[i].active && sessions[i].id == id)
            return &sessions[i];
    return NULL;
}

static client_udp_session *client_udp_create(client_udp_session *sessions, const ct_config *cfg,
                                             const ct_udp_datagram *datagram, uint64_t now) {
    const ct_service_config *svc = client_svc(cfg, datagram->service);
    if (!svc || svc->type != 2)
        return NULL;
    size_t active = 0;
    for (size_t i = 0; i < CONFIG_MAX_UDP_SESSIONS; i++)
        if (sessions[i].active)
            active++;
    if (active >= (size_t)svc->udp_max_sessions) {
        ct_metric_inc(CT_METRIC_UDP_SESSIONS_REJECTED_TOTAL);
        return NULL;
    }
    ct_udp_endpoint local;
    if (ct_udp_endpoint_from_host(&local, svc->local_addr, svc->local_port))
        return NULL;
    ct_socket fd = ct_udp_open(local.family);
    if (fd == CT_INVALID_SOCKET)
        return NULL;
    for (size_t i = 0; i < CONFIG_MAX_UDP_SESSIONS; i++)
        if (!sessions[i].active) {
            memset(&sessions[i], 0, sizeof sessions[i]);
            sessions[i].active = 1;
            sessions[i].id = datagram->session_id;
            sessions[i].peer = datagram->peer;
            sessions[i].local = local;
            sessions[i].svc = svc;
            sessions[i].fd = fd;
            sessions[i].last_ms = now;
            ct_metric_inc(CT_METRIC_UDP_SESSIONS_ACTIVE);
            ct_metric_inc(CT_METRIC_UDP_SESSIONS_CREATED_TOTAL);
            return &sessions[i];
        }
    ct_socket_close(fd);
    return NULL;
}

static void client_udp_close(client_udp_session *session, int expired) {
    if (!session->active)
        return;
    ct_socket_close(session->fd);
    memset(session, 0, sizeof *session);
    ct_metric_dec(CT_METRIC_UDP_SESSIONS_ACTIVE);
    if (expired)
        ct_metric_inc(CT_METRIC_UDP_SESSIONS_EXPIRED_TOTAL);
}

static int client_udp_from_server(client_udp_session *sessions, const ct_config *cfg,
                                  const uint8_t *payload, size_t payload_len, uint64_t now) {
    ct_udp_datagram datagram;
    if (ct_udp_datagram_unpack(payload, payload_len, &datagram) || datagram.direction != 0) {
        ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_DROPPED_TOTAL);
        return -1;
    }
    client_udp_session *session = client_udp_find(sessions, datagram.session_id);
    if (!session)
        session = client_udp_create(sessions, cfg, &datagram, now);
    if (!session || strcmp(session->svc->id, datagram.service) ||
        !endpoint_equal(&session->peer, &datagram.peer)) {
        ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_DROPPED_TOTAL);
        return 0;
    }
    if (datagram.payload_len > (size_t)session->svc->udp_max_datagram_size) {
        ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_OVERSIZED_TOTAL);
        ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_DROPPED_TOTAL);
        return 0;
    }
    if (!udp_replay_accept(&session->rx_high, &session->rx_bitmap, datagram.sequence)) {
        ct_metric_inc(CT_METRIC_UDP_REPLAY_DROPPED_TOTAL);
        return 0;
    }
    session->last_ms = now;
    if (ct_udp_send(session->fd, &session->local, datagram.payload, datagram.payload_len) < 0) {
        ct_metric_inc(CT_METRIC_UDP_LOCAL_SEND_FAILURES_TOTAL);
        return -1;
    }
    ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_C2S_TOTAL);
    ct_metric_add(CT_METRIC_UDP_BYTES_C2S_TOTAL, datagram.payload_len);
    return 0;
}

static int client_udp_from_local(client_udp_session *session, ct_control *ctl, uint64_t now) {
    uint8_t payload[CONFIG_MAX_UDP_DATAGRAM_SIZE], frame[CT_CONTROL_BUFFER_SIZE];
    ct_udp_endpoint source;
    size_t payload_len = 0, frame_len = 0;
    int rc = ct_udp_recv(session->fd, &source, payload, sizeof payload, &payload_len);
    if (rc)
        return rc < 0 ? -1 : 0;
    if (payload_len > (size_t)session->svc->udp_max_datagram_size) {
        ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_OVERSIZED_TOTAL);
        ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_DROPPED_TOTAL);
        return 0;
    }
    session->last_ms = now;
    ct_udp_datagram datagram = {1,       session->id, ++session->tx_seq, "", session->peer,
                                payload, payload_len};
    snprintf(datagram.service, sizeof datagram.service, "%s", session->svc->id);
    if (ct_udp_datagram_pack(frame, sizeof frame, &frame_len, &datagram) ||
        ct_control_send(ctl, CT_MSG_UDP_DATAGRAM, session->id, frame, frame_len, 5000)) {
        ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_DROPPED_TOTAL);
        return -1;
    }
    ct_metric_inc(CT_METRIC_UDP_DATAGRAMS_S2C_TOTAL);
    ct_metric_add(CT_METRIC_UDP_BYTES_S2C_TOTAL, payload_len);
    return 0;
}

static void client_udp_expire(client_udp_session *sessions, uint64_t now) {
    for (size_t i = 0; i < CONFIG_MAX_UDP_SESSIONS; i++)
        if (sessions[i].active &&
            now - sessions[i].last_ms > (uint64_t)sessions[i].svc->udp_idle_timeout * 1000u)
            client_udp_close(&sessions[i], 1);
}
#endif

static int add_work(const ct_config *cfg, ct_control *c, idle_work *w, size_t *n) {
    if (*n >= MAX_WORK)
        return -1;
    ct_work connection;
    if (ct_work_connect(cfg, c, &connection)) {
        ct_metric_inc(CT_METRIC_WORK_CONNECT_FAILURES_TOTAL);
        return -1;
    }
    w[*n].connection = connection;
    w[*n].read_retry_after_ms = 0;
    (*n)++;
    ct_metric_inc(CT_METRIC_WORK_IDLE);
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
        b[o++] = (uint8_t)s->proxy_protocol;
#ifdef CONFIG_FEATURE_UDP
        if (s->type == 2) {
            ct_put_u32(b + o, (uint32_t)s->udp_idle_timeout);
            o += 4;
            ct_put_u32(b + o, (uint32_t)s->udp_reply_timeout);
            o += 4;
            ct_put_u32(b + o, (uint32_t)s->udp_max_sessions);
            o += 4;
            ct_put_u32(b + o, (uint32_t)s->udp_max_datagram_size);
            o += 4;
        }
#endif
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
#ifdef CONFIG_FEATURE_UDP
    client_udp_session *udp = NULL;
#endif
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
    unsigned recommended_fd =
        (unsigned)(1u + (size_t)cfg->pool_count + 2u * (size_t)CONFIG_MAX_STREAMS + 32u);
    unsigned long long fd_soft = 0, fd_hard = 0;
    ct_fd_limit_diagnostics(recommended_fd, &fd_soft, &fd_hard);
    ct_log_status("client", "fd_limit soft=%llu hard=%llu recommended>=%u", fd_soft, fd_hard,
                  recommended_fd);
    const size_t event_capacity = 1u + MAX_WORK + 2u * MAX_RELAYS
#ifdef CONFIG_FEATURE_UDP
                                  + CONFIG_MAX_UDP_SESSIONS
#endif
        ;
    work = calloc(MAX_WORK, sizeof *work);
    relays = calloc(MAX_RELAYS, sizeof *relays);
#ifdef CONFIG_FEATURE_UDP
    udp = calloc(CONFIG_MAX_UDP_SESSIONS, sizeof *udp);
#endif
    refs = calloc(event_capacity, sizeof *refs);
    events = calloc(event_capacity, sizeof *events);
    if (!work || !relays || !refs || !events
#ifdef CONFIG_FEATURE_UDP
        || !udp
#endif
    ) {
        free(work);
        free(relays);
#ifdef CONFIG_FEATURE_UDP
        free(udp);
#endif
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
#ifdef CONFIG_FEATURE_UDP
        for (size_t i = 0; i < CONFIG_MAX_UDP_SESSIONS; i++)
            if (udp[i].active) {
                refs[rn] = (ref){REF_UDP_LOCAL, NULL, &udp[i]};
                event_loop_add(l, udp[i].fd, CT_EV_READ, &refs[rn++]);
            }
#endif
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
#ifdef CONFIG_FEATURE_UDP
                } else if (h.type == CT_MSG_UDP_DATAGRAM) {
                    if (client_udp_from_server(udp, cfg, b, n, ct_monotonic_ms()))
                        goto done;
#endif
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
                ct_stream_metadata metadata;
                uint8_t rnd[32];
                ct_frame_header start_header;
                int available = ct_plain_frame_available(iw->connection.fd, CT_CONTROL_BUFFER_SIZE,
                                                         &start_header);
                if (available < 0) {
                    ct_socket_close(iw->connection.fd);
                    work[wi] = work[--wn];
                    ct_metric_dec(CT_METRIC_WORK_IDLE);
                    continue;
                }
                if (!available) {
                    iw->read_retry_after_ms = ct_monotonic_ms() + 50;
                    continue;
                }
                ct_work wf = iw->connection;
                work[wi] = work[--wn];
                ct_metric_dec(CT_METRIC_WORK_IDLE);
                if (ct_start_stream_recv(&wf, &ctl, id, sizeof id, &sid, &em, &metadata, rnd)) {
                    CT_LOGW("client", "rejected START_STREAM on authenticated work connection");
                    ct_socket_close(wf.fd);
                    continue;
                }
                const ct_service_config *s = client_svc(cfg, id);
                if (s && s->encryption != em)
                    s = NULL;
                if (s && s->proxy_protocol != (int)metadata.proxy_protocol) {
                    ct_metric_inc(CT_METRIC_PROXY_PROTOCOL_MODE_MISMATCH_TOTAL);
                    s = NULL;
                }
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
                                      sid, rnd, wf.id, id, &metadata)) {
                        ct_socket_close(local);
                        ct_socket_close(wf.fd);
                    }
                }
#ifdef CONFIG_FEATURE_WORK_POOL
                while (wn < (size_t)cfg->pool_count && wn < MAX_WORK)
                    if (add_work(cfg, &ctl, work, &wn))
                        break;
#endif
            }
#ifdef CONFIG_FEATURE_UDP
            else if (r->kind == REF_UDP_LOCAL) {
                if (client_udp_from_local((client_udp_session *)r->ptr, &ctl, ct_monotonic_ms()))
                    goto done;
            }
#endif
            else {
                ct_relay *rr = (ct_relay *)r->ptr;
                ct_socket fd = r->kind == REF_RELAY_DIRECT ? rr->direct : rr->work;
                (void)ct_relay_process(rr, fd, events[ei].events);
            }
        }
        uint64_t now = ct_monotonic_ms();
#ifdef CONFIG_FEATURE_UDP
        client_udp_expire(udp, now);
#endif
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
    for (size_t i = 0; i < wn; i++)
        ct_metric_dec(CT_METRIC_WORK_IDLE);
    for (size_t i = 0; i < MAX_RELAYS; i++)
        if (!relays[i].closed)
            ct_relay_close(&relays[i]);
#ifdef CONFIG_FEATURE_UDP
    for (size_t i = 0; i < CONFIG_MAX_UDP_SESSIONS; i++)
        client_udp_close(&udp[i], 0);
#endif
    ct_socket_close(ctl.fd);
    ct_secure_zero(&ctl.keys, sizeof ctl.keys);
    free(work);
    free(relays);
#ifdef CONFIG_FEATURE_UDP
    free(udp);
#endif
    free(refs);
    free(events);
    ct_metrics_log_snapshot("client");
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
        ct_metric_inc(CT_METRIC_CONTROL_RECONNECTS_TOTAL);
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
