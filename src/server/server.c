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
#define MAX_PENDING CONFIG_MAX_PENDING_STREAMS

typedef struct server_peer server_peer;
typedef struct {
    ct_service_config cfg;
    ct_socket listen_fd;
} server_service;
typedef struct {
    ct_socket fd;
    server_service *svc;
    uint64_t deadline;
} pending_conn;
struct server_peer {
    int active;
    ct_control ctl;
    const ct_authorized_client *auth;
    server_service svc[CT_MAX_SERVICES];
    size_t svc_n;
    ct_socket work[MAX_WORK];
    size_t work_n;
    pending_conn pending[MAX_PENDING];
    size_t pending_n;
    ct_relay relays[MAX_RELAYS];
    uint64_t next_stream;
};
enum { REF_ACCEPT, REF_CONTROL, REF_SERVICE, REF_WORK, REF_RELAY_DIRECT, REF_RELAY_WORK };
typedef struct {
    int kind;
    server_peer *peer;
    void *ptr;
} ref;
typedef struct {
    char address[CT_MAX_ADDR + 1];
    uint64_t blocked_until;
    unsigned failures;
} auth_rate;
static auth_rate *auth_rate_slot(auth_rate *rates, size_t count, const char *address) {
    auth_rate *empty = NULL, *oldest = &rates[0];
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(rates[i].address, address))
            return &rates[i];
        if (!rates[i].address[0] && !empty)
            empty = &rates[i];
        if (rates[i].blocked_until < oldest->blocked_until)
            oldest = &rates[i];
    }
    auth_rate *slot = empty ? empty : oldest;
    memset(slot, 0, sizeof *slot);
    snprintf(slot->address, sizeof slot->address, "%s", address);
    return slot;
}
static void auth_rate_failed(auth_rate *rate, uint64_t now) {
    if (rate->failures < 5)
        rate->failures++;
    rate->blocked_until = now + ((uint64_t)1 << rate->failures) * 500u;
}
static void peer_close(server_peer *p) {
    if (!p->active)
        return;
    (void)ct_control_send(&p->ctl, CT_MSG_GOAWAY, 0, NULL, 0, 100);
    ct_socket_close(p->ctl.fd);
    for (size_t i = 0; i < p->svc_n; i++)
        ct_socket_close(p->svc[i].listen_fd);
    for (size_t i = 0; i < p->work_n; i++)
        ct_socket_close(p->work[i]);
    for (size_t i = 0; i < p->pending_n; i++)
        ct_socket_close(p->pending[i].fd);
    for (size_t i = 0; i < MAX_RELAYS; i++)
        if (!p->relays[i].closed)
            ct_relay_close(&p->relays[i]);
    ct_secure_zero(&p->ctl.keys, sizeof p->ctl.keys);
    memset(p, 0, sizeof *p);
    for (size_t i = 0; i < MAX_RELAYS; i++) {
        p->relays[i].closed = 1;
        p->relays[i].direct = CT_INVALID_SOCKET;
        p->relays[i].work = CT_INVALID_SOCKET;
    }
}
static server_peer *find_session(server_peer *p, size_t n, uint64_t sid) {
    for (size_t i = 0; i < n; i++)
        if (p[i].active && p[i].ctl.keys.session_id == sid)
            return &p[i];
    return NULL;
}
static server_service *find_service(server_peer *p, const char *id) {
    for (size_t i = 0; i < p->svc_n; i++)
        if (!strcmp(p->svc[i].cfg.id, id))
            return &p->svc[i];
    return NULL;
}
static size_t active_streams(const server_peer *p) {
    size_t n = p->pending_n;
    for (size_t i = 0; i < MAX_RELAYS; i++)
        if (!p->relays[i].closed)
            n++;
    return n;
}
static int register_service(server_peer *p, const ct_config *cfg, const uint8_t *b, size_t n) {
    size_t o = 0;
    ct_service_config s;
    memset(&s, 0, sizeof s);
    if (ct_unpack_string(b, n, &o, s.id, sizeof s.id) ||
        ct_unpack_string(b, n, &o, s.remote_addr, sizeof s.remote_addr) || n - o != 4)
        return -1;
    s.remote_port = ct_get_u16(b + o);
    o += 2;
    s.type = b[o++];
    s.encryption = (ct_enc_mode)b[o++];
    uint8_t reply[CT_CONTROL_BUFFER_SIZE];
    size_t z = 0;
    int ok = 1;
    const char *why = "";
    if (s.type != 1) {
        ok = 0;
        why = "UNSUPPORTED_SERVICE_TYPE";
    } else if (s.encryption != CT_ENC_REQUIRED && s.encryption != CT_ENC_DISABLED) {
        ok = 0;
        why = "INVALID_ENCRYPTION";
#ifndef CONFIG_FEATURE_DATA_ENCRYPTION
    } else if (s.encryption == CT_ENC_REQUIRED) {
        ok = 0;
        why = "DATA_ENCRYPTION_UNAVAILABLE";
#endif
    } else if (find_service(p, s.id)) {
        ok = 0;
        why = "DUPLICATE_SERVICE";
    } else if (p->svc_n >= (size_t)p->auth->max_services ||
               p->svc_n >= (size_t)cfg->max_services_per_client) {
        ok = 0;
        why = "SERVICE_LIMIT";
    } else if (!ct_authorized_port(p->auth, s.remote_addr, s.remote_port)) {
        ok = 0;
        why = "NOT_AUTHORIZED";
    }
    ct_socket l = CT_INVALID_SOCKET;
    if (ok && (l = ct_net_listen(s.remote_addr, s.remote_port, 128)) == CT_INVALID_SOCKET) {
        ok = 0;
        why = "BIND_FAILED";
    }
    ct_pack_string(reply, sizeof reply, &z, s.id, CT_MAX_SERVICE_ID);
    if (ok) {
        server_service *ss = &p->svc[p->svc_n++];
        memset(ss, 0, sizeof *ss);
        ss->cfg = s;
        ss->listen_fd = l;
        CT_LOGI("server", "client_id=%s service_id=%s listening=%s:%u", p->ctl.client_id, s.id,
                s.remote_addr, s.remote_port);
        return ct_control_send(&p->ctl, CT_MSG_REGISTER_OK, 0, reply, z, 5000);
    }
    ct_pack_string(reply, sizeof reply, &z, why, 80);
    CT_LOGW("server", "client_id=%s service_id=%s registration failed: %s", p->ctl.client_id, s.id,
            why);
    return ct_control_send(&p->ctl, CT_MSG_REGISTER_FAILED, 0, reply, z, 5000);
}
static int start_relay(server_peer *p, pending_conn pc, ct_socket work) {
    ct_relay *r = NULL;
    for (size_t i = 0; i < MAX_RELAYS; i++)
        if (p->relays[i].closed || p->relays[i].direct == CT_INVALID_SOCKET) {
            r = &p->relays[i];
            break;
        }
    if (!r)
        return -1;
    uint64_t sid = ++p->next_stream;
    if (!sid)
        sid = ++p->next_stream;
    uint8_t rnd[32];
    if (ct_start_stream_send(work, &p->ctl, pc.svc->cfg.id, sid, pc.svc->cfg.encryption, rnd))
        return -1;
    int ok = 0;
    if (ct_stream_ready_recv(work, &p->ctl, sid, rnd, &ok) || !ok)
        return -1;
    if (ct_relay_init(r, pc.fd, work, 0, pc.svc->cfg.encryption, p->ctl.cipher,
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
                      p->ctl.keys.data_master,
#else
                      NULL,
#endif
                      sid, rnd))
        return -1;
    CT_LOGD("server", "client_id=%s service_id=%s stream_id=%llu started", p->ctl.client_id,
            pc.svc->cfg.id, (unsigned long long)sid);
    return 0;
}
static void match_pending(server_peer *p) {
    while (p->pending_n && p->work_n) {
        pending_conn pc = p->pending[0];
        memmove(p->pending, p->pending + 1, (--p->pending_n) * sizeof *p->pending);
        ct_socket w = p->work[--p->work_n];
        if (start_relay(p, pc, w)) {
            ct_socket_close(pc.fd);
            ct_socket_close(w);
        }
    }
}
static int server_control(server_peer *p, const ct_config *cfg) {
    uint8_t b[CT_CONTROL_BUFFER_SIZE];
    ct_frame_header h;
    size_t n;
    if (ct_control_recv(&p->ctl, &h, b, sizeof b, &n, 5000))
        return -1;
    if (h.type == CT_MSG_REGISTER_SERVICE)
        return register_service(p, cfg, b, n);
    if (h.type == CT_MSG_PING)
        return ct_control_send(&p->ctl, CT_MSG_PONG, 0, b, n, 5000);
    if (h.type == CT_MSG_PONG)
        return 0;
    if (h.type == CT_MSG_GOAWAY)
        return -1;
    return 0;
}
static int first_type(ct_socket f, uint8_t *t, uint64_t *sid) {
    uint8_t h[CT_FRAME_HEADER_SIZE];
    int z = 0;
    for (int i = 0; i < 500; i++) {
#ifdef _WIN32
        z = recv(f, (char *)h, sizeof h, MSG_PEEK);
#else
        z = (int)recv(f, h, sizeof h, MSG_PEEK);
#endif
        if (z >= (int)sizeof h)
            break;
        if (z == 0)
            return -1;
        if (z < 0 && !ct_socket_would_block())
            return -1;
        ct_sleep_ms(10);
    }
    if (z < (int)sizeof h)
        return -1;
    ct_frame_header fh;
    if (ct_frame_header_decode(h, &fh))
        return -1;
    *t = fh.type;
    *sid = fh.session_id;
    return 0;
}
int ct_run_server(const ct_config *cfg) {
    ct_runtime_init();
    const size_t event_capacity =
        1u + (size_t)CT_MAX_AUTH_CLIENTS * (1u + CT_MAX_SERVICES + 2u * MAX_RELAYS);
    auth_rate *rates = calloc(CONFIG_MAX_CLIENTS * 2u, sizeof *rates);
    server_peer *peers = calloc(CT_MAX_AUTH_CLIENTS, sizeof *peers);
    ref *refs = calloc(event_capacity, sizeof *refs);
    ct_event *events = calloc(event_capacity, sizeof *events);
    if (!rates || !peers || !refs || !events) {
        free(rates);
        free(peers);
        free(refs);
        free(events);
        return 1;
    }
    for (size_t i = 0; i < CT_MAX_AUTH_CLIENTS; i++)
        for (size_t j = 0; j < MAX_RELAYS; j++) {
            peers[i].relays[j].closed = 1;
            peers[i].relays[j].direct = CT_INVALID_SOCKET;
            peers[i].relays[j].work = CT_INVALID_SOCKET;
        }
    ct_socket listener = ct_net_listen(cfg->bind_addr, cfg->bind_port, 128);
    if (listener == CT_INVALID_SOCKET) {
        CT_LOGE("server", "cannot listen on %s:%u", cfg->bind_addr, cfg->bind_port);
        free(rates);
        free(peers);
        free(refs);
        free(events);
        return 1;
    }
    ct_log_status("server",
                  "started control=%s:%u clients=%d services_per_client=%d streams_per_client=%d "
                  "pending=%d log_file=%s",
                  cfg->bind_addr, cfg->bind_port, cfg->max_clients,
                  cfg->max_services_per_client, cfg->max_streams_per_client,
                  cfg->max_pending_streams, cfg->log_file[0] ? cfg->log_file : "stderr");
    CT_LOGI("server", "control listening on %s:%u", cfg->bind_addr, cfg->bind_port);
    while (!ct_runtime_should_stop()) {
        ct_event_loop *loop = event_loop_create(event_capacity);
        if (!loop)
            break;
        size_t rn = 0;
        refs[rn] = (ref){REF_ACCEPT, NULL, NULL};
        event_loop_add(loop, listener, CT_EV_READ, &refs[rn++]);
        for (size_t i = 0; i < CT_MAX_AUTH_CLIENTS; i++) {
            server_peer *p = &peers[i];
            if (!p->active)
                continue;
            refs[rn] = (ref){REF_CONTROL, p, NULL};
            event_loop_add(loop, p->ctl.fd, CT_EV_READ, &refs[rn++]);
            for (size_t j = 0; j < p->svc_n; j++) {
                refs[rn] = (ref){REF_SERVICE, p, &p->svc[j]};
                event_loop_add(loop, p->svc[j].listen_fd, CT_EV_READ, &refs[rn++]);
            }
            for (size_t j = 0; j < MAX_RELAYS; j++)
                if (!p->relays[j].closed) {
                    int ev = ct_relay_events(&p->relays[j], p->relays[j].direct);
                    refs[rn] = (ref){REF_RELAY_DIRECT, p, &p->relays[j]};
                    event_loop_add(loop, p->relays[j].direct, ev, &refs[rn++]);
                    ev = ct_relay_events(&p->relays[j], p->relays[j].work);
                    refs[rn] = (ref){REF_RELAY_WORK, p, &p->relays[j]};
                    event_loop_add(loop, p->relays[j].work, ev, &refs[rn++]);
                }
        }
        int ne = event_loop_wait(loop, events, event_capacity, 500);
        uint64_t now = ct_monotonic_ms();
        for (int ei = 0; ei < ne; ei++) {
            ref *r = (ref *)events[ei].user;
            if (r->kind == REF_ACCEPT) {
                char remote[CT_MAX_ADDR + 1] = "?";
                ct_socket f = ct_net_accept(listener, remote, sizeof remote);
                if (f == CT_INVALID_SOCKET)
                    continue;
                uint8_t typ;
                uint64_t sid;
                if (first_type(f, &typ, &sid)) {
                    ct_socket_close(f);
                    continue;
                }
                if (typ == CT_MSG_CLIENT_HELLO) {
                    auth_rate *rate = auth_rate_slot(rates, CONFIG_MAX_CLIENTS * 2, remote);
                    if (ct_monotonic_ms() < rate->blocked_until) {
                        CT_LOGW("server", "authentication rate-limited remote=%s", remote);
                        ct_socket_close(f);
                        continue;
                    }
                    server_peer *slot = NULL;
                    for (size_t i = 0; i < (size_t)cfg->max_clients && i < CT_MAX_AUTH_CLIENTS; i++)
                        if (!peers[i].active) {
                            slot = &peers[i];
                            break;
                        }
                    if (!slot || ct_handshake_server(f, cfg, &slot->ctl, &slot->auth)) {
                        CT_LOGW("server", "authentication rejected remote=%s", remote);
                        auth_rate_failed(rate, ct_monotonic_ms());
                        ct_socket_close(f);
                    } else {
                        memset(rate, 0, sizeof *rate);
                        slot->active = 1;
                        slot->next_stream = ct_monotonic_ms();
                        CT_LOGI("server", "authenticated client_id=%s remote=%s cipher=%s",
                                slot->ctl.client_id, remote,
                                slot->ctl.cipher == CT_CIPHER_CHACHA ? "xchacha20-poly1305"
                                                                     : "unsupported");
                    }
                } else if (typ == CT_MSG_WORK_CONNECTION_BIND) {
                    server_peer *p = find_session(peers, CT_MAX_AUTH_CLIENTS, sid);
                    if (!p || p->work_n == MAX_WORK || ct_work_accept_bind(f, &p->ctl))
                        ct_socket_close(f);
                    else {
                        p->work[p->work_n++] = f;
                        match_pending(p);
                    }
                } else
                    ct_socket_close(f);
            } else if (r->kind == REF_CONTROL) {
                if (server_control(r->peer, cfg))
                    peer_close(r->peer);
            } else if (r->kind == REF_SERVICE) {
                server_peer *p = r->peer;
                server_service *s = (server_service *)r->ptr;
                ct_socket f = ct_net_accept(s->listen_fd, NULL, 0);
                if (f != CT_INVALID_SOCKET) {
                    if (p->pending_n >= (size_t)cfg->max_pending_streams ||
                        p->pending_n == MAX_PENDING ||
                        active_streams(p) >= (size_t)p->auth->max_streams ||
                        active_streams(p) >= (size_t)cfg->max_streams_per_client) {
                        ct_socket_close(f);
                    } else {
                        p->pending[p->pending_n++] =
                            (pending_conn){f, s, now + (uint64_t)cfg->connect_timeout * 1000u};
                        if (!p->work_n)
                            (void)ct_control_send(&p->ctl, CT_MSG_REQUEST_WORK_CONNECTION, 0, NULL,
                                                  0, 1000);
                        match_pending(p);
                    }
                }
            } else {
                ct_relay *rr = (ct_relay *)r->ptr;
                ct_socket fd = r->kind == REF_RELAY_DIRECT ? rr->direct : rr->work;
                (void)ct_relay_process(rr, fd, events[ei].events);
            }
        }
        event_loop_destroy(loop);
        now = ct_monotonic_ms();
        for (size_t i = 0; i < CT_MAX_AUTH_CLIENTS; i++) {
            server_peer *p = &peers[i];
            if (!p->active)
                continue;
            for (size_t j = 0; j < p->pending_n;) {
                if (p->pending[j].deadline < now) {
                    ct_socket_close(p->pending[j].fd);
                    p->pending[j] = p->pending[--p->pending_n];
                } else
                    j++;
            }
            if (now - p->ctl.last_rx_ms > (uint64_t)cfg->heartbeat_timeout * 1000u) {
                CT_LOGW("server", "heartbeat timeout client_id=%s", p->ctl.client_id);
                peer_close(p);
            } else if (now - p->ctl.last_tx_ms > (uint64_t)cfg->heartbeat_interval * 1000u) {
                uint8_t ping[16];
                ct_put_u64(ping, p->ctl.tx_seq + 1);
                ct_put_u64(ping + 8, now);
                if (ct_control_send(&p->ctl, CT_MSG_PING, 0, ping, sizeof ping, 1000))
                    peer_close(p);
            }
        }
    }
    for (size_t i = 0; i < CT_MAX_AUTH_CLIENTS; i++)
        peer_close(&peers[i]);
    ct_socket_close(listener);
    free(rates);
    free(peers);
    free(refs);
    free(events);
    return 0;
}
