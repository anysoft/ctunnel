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
#define MAX_INCOMING (CONFIG_MAX_CLIENTS * 2U)

typedef struct server_peer server_peer;
typedef struct {
    ct_service_config cfg;
    ct_socket listen_fd[2];
    size_t listen_n;
} server_service;
typedef struct {
    ct_socket fd[2];
    size_t n;
} listen_set;
typedef struct {
    ct_socket fd;
    server_service *svc;
    uint64_t deadline;
} pending_conn;
typedef struct {
    pending_conn pending;
    ct_work work;
    uint64_t stream_id;
    uint64_t deadline;
    uint8_t random[32];
    uint64_t read_retry_after_ms;
} starting_conn;
typedef struct {
    ct_socket fd;
    int waiting_for_auth;
    uint64_t deadline;
    uint64_t read_retry_after_ms;
    char remote[CT_MAX_ADDR + 1];
    ct_server_handshake handshake;
} incoming_conn;
struct server_peer {
    int active;
    ct_control ctl;
    const ct_authorized_client *auth;
    server_service svc[CT_MAX_SERVICES];
    size_t svc_n;
    ct_work work[MAX_WORK];
    size_t work_n;
    pending_conn pending[MAX_PENDING];
    size_t pending_n;
    starting_conn starting[MAX_PENDING];
    size_t starting_n;
    ct_relay relays[MAX_RELAYS];
    uint64_t next_stream;
};
enum {
    REF_ACCEPT,
    REF_INCOMING,
    REF_CONTROL,
    REF_SERVICE,
    REF_WORK,
    REF_STARTING,
    REF_RELAY_DIRECT,
    REF_RELAY_WORK
};
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
static incoming_conn *find_incoming(incoming_conn *incoming, size_t count, ct_socket fd) {
    for (size_t i = 0; i < count; i++)
        if (incoming[i].fd == fd)
            return &incoming[i];
    return NULL;
}
static size_t incoming_for_remote(const incoming_conn *incoming, size_t count, const char *remote) {
    size_t matches = 0;
    for (size_t i = 0; i < count; i++)
        if (!strcmp(incoming[i].remote, remote))
            matches++;
    return matches;
}
static void remove_incoming(incoming_conn *incoming, size_t *count, incoming_conn *connection,
                            int close_socket) {
    size_t index = (size_t)(connection - incoming);
    if (close_socket)
        ct_socket_close(connection->fd);
    ct_handshake_server_abort(&connection->handshake);
    incoming[index] = incoming[--*count];
}
static void peer_close(server_peer *p) {
    if (!p->active)
        return;
    (void)ct_control_send(&p->ctl, CT_MSG_GOAWAY, 0, NULL, 0, 100);
    ct_socket_close(p->ctl.fd);
    for (size_t i = 0; i < p->svc_n; i++)
        for (size_t j = 0; j < p->svc[i].listen_n; j++)
            ct_socket_close(p->svc[i].listen_fd[j]);
    for (size_t i = 0; i < p->work_n; i++)
        ct_socket_close(p->work[i].fd);
    for (size_t i = 0; i < p->pending_n; i++)
        ct_socket_close(p->pending[i].fd);
    for (size_t i = 0; i < p->starting_n; i++) {
        ct_socket_close(p->starting[i].pending.fd);
        ct_socket_close(p->starting[i].work.fd);
    }
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

static int listen_set_open(listen_set *set, const char *addr, uint16_t port, int backlog) {
    memset(set, 0, sizeof *set);
    for (size_t i = 0; i < sizeof set->fd / sizeof set->fd[0]; i++)
        set->fd[i] = CT_INVALID_SOCKET;
    if (!strcmp(addr, "*")) {
#ifdef CONFIG_FEATURE_IPV4
        set->fd[set->n] = ct_net_listen("0.0.0.0", port, backlog);
        if (set->fd[set->n] != CT_INVALID_SOCKET)
            set->n++;
#endif
#ifdef CONFIG_FEATURE_IPV6
        set->fd[set->n] = ct_net_listen("::", port, backlog);
        if (set->fd[set->n] != CT_INVALID_SOCKET)
            set->n++;
#endif
        if (set->n)
            return 0;
        return -1;
    }
    set->fd[0] = ct_net_listen(addr, port, backlog);
    if (set->fd[0] == CT_INVALID_SOCKET)
        return -1;
    set->n = 1;
    return 0;
}

static void listen_set_close(listen_set *set) {
    for (size_t i = 0; i < set->n; i++)
        ct_socket_close(set->fd[i]);
    memset(set, 0, sizeof *set);
}
static int listen_set_overlaps_existing(const listen_set *set, const listen_set *control,
                                        const server_peer *peers, size_t peer_count) {
    for (size_t i = 0; i < set->n; i++) {
        for (size_t j = 0; j < control->n; j++)
            if (ct_net_bound_endpoints_overlap(set->fd[i], control->fd[j]))
                return 1;
        for (size_t p = 0; p < peer_count; p++) {
            if (!peers[p].active)
                continue;
            for (size_t s = 0; s < peers[p].svc_n; s++)
                for (size_t j = 0; j < peers[p].svc[s].listen_n; j++)
                    if (ct_net_bound_endpoints_overlap(set->fd[i], peers[p].svc[s].listen_fd[j]))
                        return 1;
        }
    }
    return 0;
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
    size_t n = p->pending_n + p->starting_n;
    for (size_t i = 0; i < MAX_RELAYS; i++)
        if (!p->relays[i].closed)
            n++;
    return n;
}
static int register_service(server_peer *p, const ct_config *cfg, const listen_set *control,
                            const server_peer *peers, size_t peer_count, const uint8_t *b,
                            size_t n) {
    ct_service_config s;
    memset(&s, 0, sizeof s);
    uint8_t type, encryption;
    if (ct_register_request_decode(b, n, s.id, sizeof s.id, s.remote_addr, sizeof s.remote_addr,
                                   &s.remote_port, &type, &encryption))
        return -1;
    s.type = type;
    s.encryption = (ct_enc_mode)encryption;
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
    listen_set listeners;
    memset(&listeners, 0, sizeof listeners);
    if (ok && listen_set_open(&listeners, s.remote_addr, s.remote_port, 128)) {
        ok = 0;
        why = "BIND_FAILED";
    } else if (ok && listen_set_overlaps_existing(&listeners, control, peers, peer_count)) {
        ok = 0;
        why = "LISTENER_CONFLICT";
    }
    ct_pack_string(reply, sizeof reply, &z, s.id, CT_MAX_SERVICE_ID);
    if (ok) {
        server_service *ss = &p->svc[p->svc_n++];
        memset(ss, 0, sizeof *ss);
        ss->cfg = s;
        ss->listen_n = listeners.n;
        for (size_t i = 0; i < listeners.n; i++) {
            ss->listen_fd[i] = listeners.fd[i];
            listeners.fd[i] = CT_INVALID_SOCKET;
        }
        CT_LOGI("server", "client_id=%s service_id=%s listening=%s:%u", p->ctl.client_id, s.id,
                s.remote_addr, s.remote_port);
        return ct_control_send(&p->ctl, CT_MSG_REGISTER_OK, 0, reply, z, 5000);
    }
    listen_set_close(&listeners);
    ct_pack_string(reply, sizeof reply, &z, why, 80);
    CT_LOGW("server", "client_id=%s service_id=%s registration failed: %s", p->ctl.client_id, s.id,
            why);
    return ct_control_send(&p->ctl, CT_MSG_REGISTER_FAILED, 0, reply, z, 5000);
}
static int start_relay_begin(server_peer *p, pending_conn pending, ct_work work,
                             uint64_t timeout_ms) {
    if (p->starting_n == MAX_PENDING)
        return -1;
    uint64_t sid = ++p->next_stream;
    if (!sid)
        sid = ++p->next_stream;
    starting_conn starting;
    memset(&starting, 0, sizeof starting);
    starting.pending = pending;
    starting.work = work;
    starting.stream_id = sid;
    starting.deadline = ct_monotonic_ms() + timeout_ms;
    if (ct_start_stream_send(&work, &p->ctl, pending.svc->cfg.id, sid, pending.svc->cfg.encryption,
                             starting.random))
        return -1;
    p->starting[p->starting_n++] = starting;
    return 0;
}
static int start_relay_finish(server_peer *p, starting_conn *starting) {
    ct_relay *r = NULL;
    for (size_t i = 0; i < MAX_RELAYS; i++)
        if (p->relays[i].closed || p->relays[i].direct == CT_INVALID_SOCKET) {
            r = &p->relays[i];
            break;
        }
    if (!r)
        return -1;
    int ok = 0;
    if (ct_stream_ready_recv(&starting->work, &p->ctl, starting->pending.svc->cfg.id,
                             starting->pending.svc->cfg.encryption, starting->stream_id,
                             starting->random, &ok) ||
        !ok)
        return -1;
    if (ct_relay_init(r, starting->pending.fd, starting->work.fd, 0,
                      starting->pending.svc->cfg.encryption, p->ctl.cipher,
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
                      p->ctl.keys.data_master,
#else
                      NULL,
#endif
                      starting->stream_id, starting->random, starting->work.id,
                      starting->pending.svc->cfg.id))
        return -1;
    CT_LOGD("server", "client_id=%s service_id=%s stream_id=%llu started", p->ctl.client_id,
            starting->pending.svc->cfg.id, (unsigned long long)starting->stream_id);
    return 0;
}
static void remove_starting(server_peer *p, size_t index, int close_sockets) {
    if (close_sockets) {
        ct_socket_close(p->starting[index].pending.fd);
        ct_socket_close(p->starting[index].work.fd);
    }
    p->starting[index] = p->starting[--p->starting_n];
}
static starting_conn *find_starting(server_peer *p, ct_socket work, size_t *index) {
    for (size_t i = 0; i < p->starting_n; i++)
        if (p->starting[i].work.fd == work) {
            *index = i;
            return &p->starting[i];
        }
    return NULL;
}
static void match_pending(server_peer *p, const ct_config *cfg) {
    while (p->pending_n && p->work_n && p->starting_n < MAX_PENDING) {
        pending_conn pc = p->pending[0];
        memmove(p->pending, p->pending + 1, (--p->pending_n) * sizeof *p->pending);
        ct_work w = p->work[--p->work_n];
        if (start_relay_begin(p, pc, w, (uint64_t)cfg->connect_timeout * UINT64_C(1000))) {
            ct_socket_close(pc.fd);
            ct_socket_close(w.fd);
        }
    }
}
static int server_control(server_peer *p, const ct_config *cfg, const listen_set *control,
                          const server_peer *peers, size_t peer_count) {
    uint8_t b[CT_CONTROL_BUFFER_SIZE];
    ct_frame_header h;
    size_t n;
    if (ct_control_recv(&p->ctl, &h, b, sizeof b, &n, 5000))
        return -1;
    if (h.type == CT_MSG_REGISTER_SERVICE)
        return register_service(p, cfg, control, peers, peer_count, b, n);
    if (h.type == CT_MSG_PING)
        return ct_control_send(&p->ctl, CT_MSG_PONG, 0, b, n, 5000);
    if (h.type == CT_MSG_PONG)
        return 0;
    if (h.type == CT_MSG_GOAWAY)
        return -1;
    return -1;
}
int ct_run_server(const ct_config *cfg) {
    ct_runtime_init();
    const size_t event_capacity =
        2u + MAX_INCOMING +
        (size_t)CT_MAX_AUTH_CLIENTS * (1u + 2u * CT_MAX_SERVICES + MAX_PENDING + 2u * MAX_RELAYS);
    auth_rate *rates = calloc(CONFIG_MAX_CLIENTS * 2u, sizeof *rates);
    incoming_conn *incoming = calloc(MAX_INCOMING, sizeof *incoming);
    size_t incoming_n = 0;
    server_peer *peers = calloc(CT_MAX_AUTH_CLIENTS, sizeof *peers);
    ref *refs = calloc(event_capacity, sizeof *refs);
    ct_event *events = calloc(event_capacity, sizeof *events);
    if (!rates || !incoming || !peers || !refs || !events) {
        free(rates);
        free(incoming);
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
    listen_set control_listeners;
    if (listen_set_open(&control_listeners, cfg->bind_addr, cfg->bind_port, 128)) {
        CT_LOGE("server", "cannot listen on %s:%u", cfg->bind_addr, cfg->bind_port);
        free(rates);
        free(incoming);
        free(peers);
        free(refs);
        free(events);
        return 1;
    }
    ct_log_status("server",
                  "started control=%s:%u clients=%d services_per_client=%d streams_per_client=%d "
                  "pending=%d log_file=%s",
                  cfg->bind_addr, cfg->bind_port, cfg->max_clients, cfg->max_services_per_client,
                  cfg->max_streams_per_client, cfg->max_pending_streams,
                  cfg->log_file[0] ? cfg->log_file : "stderr");
    CT_LOGI("server", "control listening on %s:%u", cfg->bind_addr, cfg->bind_port);
    while (!ct_runtime_should_stop()) {
        ct_event_loop *loop = event_loop_create(event_capacity);
        if (!loop)
            break;
        size_t rn = 0;
        for (size_t i = 0; i < control_listeners.n; i++) {
            refs[rn] = (ref){REF_ACCEPT, NULL, (void *)(uintptr_t)control_listeners.fd[i]};
            event_loop_add(loop, control_listeners.fd[i], CT_EV_READ, &refs[rn++]);
        }
        for (size_t i = 0; i < incoming_n; i++) {
            if (ct_monotonic_ms() < incoming[i].read_retry_after_ms)
                continue;
            refs[rn] = (ref){REF_INCOMING, NULL, (void *)(uintptr_t)incoming[i].fd};
            event_loop_add(loop, incoming[i].fd, CT_EV_READ, &refs[rn++]);
        }
        for (size_t i = 0; i < CT_MAX_AUTH_CLIENTS; i++) {
            server_peer *p = &peers[i];
            if (!p->active)
                continue;
            if (ct_monotonic_ms() >= p->ctl.read_retry_after_ms) {
                refs[rn] = (ref){REF_CONTROL, p, NULL};
                event_loop_add(loop, p->ctl.fd, CT_EV_READ, &refs[rn++]);
            }
            for (size_t j = 0; j < p->svc_n; j++) {
                for (size_t k = 0; k < p->svc[j].listen_n; k++) {
                    refs[rn] = (ref){REF_SERVICE, p, &p->svc[j]};
                    event_loop_add(loop, p->svc[j].listen_fd[k], CT_EV_READ, &refs[rn++]);
                }
            }
            for (size_t j = 0; j < p->starting_n; j++) {
                if (ct_monotonic_ms() < p->starting[j].read_retry_after_ms)
                    continue;
                refs[rn] = (ref){REF_STARTING, p, (void *)(uintptr_t)p->starting[j].work.fd};
                event_loop_add(loop, p->starting[j].work.fd, CT_EV_READ, &refs[rn++]);
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
        int ne = event_loop_wait(loop, events, event_capacity, 50);
        uint64_t now = ct_monotonic_ms();
        for (int ei = 0; ei < ne; ei++) {
            ref *r = (ref *)events[ei].user;
            if (r->kind == REF_ACCEPT) {
                char remote[CT_MAX_ADDR + 1] = "?";
                ct_socket f = ct_net_accept((ct_socket)(uintptr_t)r->ptr, remote, sizeof remote);
                if (f == CT_INVALID_SOCKET)
                    continue;
                if (incoming_n == MAX_INCOMING ||
                    incoming_for_remote(incoming, incoming_n, remote) >= 2) {
                    ct_socket_close(f);
                    continue;
                }
                incoming_conn *connection = &incoming[incoming_n++];
                memset(connection, 0, sizeof *connection);
                connection->fd = f;
                connection->deadline = now + (uint64_t)cfg->handshake_timeout * UINT64_C(1000);
                snprintf(connection->remote, sizeof connection->remote, "%s", remote);
            } else if (r->kind == REF_INCOMING) {
                ct_socket fd = (ct_socket)(uintptr_t)r->ptr;
                incoming_conn *connection = find_incoming(incoming, incoming_n, fd);
                if (!connection)
                    continue;
                ct_frame_header first;
                int available = ct_plain_frame_available(fd, CT_CONTROL_BUFFER_SIZE, &first);
                if (available <= 0) {
                    if (available < 0)
                        remove_incoming(incoming, &incoming_n, connection, 1);
                    else
                        connection->read_retry_after_ms = ct_monotonic_ms() + 50;
                    continue;
                }
                if (!connection->waiting_for_auth && first.type == CT_MSG_CLIENT_HELLO) {
                    auth_rate *rate =
                        auth_rate_slot(rates, CONFIG_MAX_CLIENTS * 2, connection->remote);
                    if (ct_monotonic_ms() < rate->blocked_until) {
                        CT_LOGW("server", "authentication rate-limited remote=%s",
                                connection->remote);
                        remove_incoming(incoming, &incoming_n, connection, 1);
                        continue;
                    }
                    if (first.session_id || first.stream_id || first.sequence ||
                        first.payload_len < 70 || first.payload_len > 2U + CT_MAX_CLIENT_ID + 68U ||
                        ct_handshake_server_begin(fd, cfg, &connection->handshake)) {
                        CT_LOGW("server", "authentication rejected remote=%s", connection->remote);
                        auth_rate_failed(rate, ct_monotonic_ms());
                        remove_incoming(incoming, &incoming_n, connection, 1);
                        continue;
                    }
                    connection->waiting_for_auth = 1;
                } else if (!connection->waiting_for_auth &&
                           first.type == CT_MSG_WORK_CONNECTION_BIND) {
                    server_peer *p = find_session(peers, CT_MAX_AUTH_CLIENTS, first.session_id);
                    ct_work accepted_work;
                    if (!p || first.stream_id || first.sequence || first.payload_len != 64 ||
                        p->work_n == MAX_WORK || ct_work_accept_bind(fd, &p->ctl, &accepted_work)) {
                        remove_incoming(incoming, &incoming_n, connection, 1);
                    } else {
                        p->work[p->work_n++] = accepted_work;
                        remove_incoming(incoming, &incoming_n, connection, 0);
                        match_pending(p, cfg);
                    }
                } else if (connection->waiting_for_auth && first.type == CT_MSG_CLIENT_AUTH &&
                           !first.session_id && !first.stream_id && !first.sequence &&
                           first.payload_len == 64) {
                    server_peer *slot = NULL;
                    for (size_t i = 0; i < (size_t)cfg->max_clients && i < CT_MAX_AUTH_CLIENTS; i++)
                        if (!peers[i].active) {
                            slot = &peers[i];
                            break;
                        }
                    auth_rate *rate =
                        auth_rate_slot(rates, CONFIG_MAX_CLIENTS * 2, connection->remote);
                    if (!slot || ct_handshake_server_finish(fd, cfg, &connection->handshake,
                                                            &slot->ctl, &slot->auth)) {
                        CT_LOGW("server", "authentication rejected remote=%s", connection->remote);
                        auth_rate_failed(rate, ct_monotonic_ms());
                        remove_incoming(incoming, &incoming_n, connection, 1);
                    } else {
                        char authenticated_remote[CT_MAX_ADDR + 1];
                        snprintf(authenticated_remote, sizeof authenticated_remote, "%s",
                                 connection->remote);
                        memset(rate, 0, sizeof *rate);
                        slot->active = 1;
                        slot->next_stream = ct_monotonic_ms();
                        remove_incoming(incoming, &incoming_n, connection, 0);
                        CT_LOGI("server", "authenticated client_id=%s remote=%s cipher=%s",
                                slot->ctl.client_id, authenticated_remote,
                                slot->ctl.cipher == CT_CIPHER_CHACHA ? "xchacha20-poly1305"
                                                                     : "unsupported");
                    }
                } else {
                    remove_incoming(incoming, &incoming_n, connection, 1);
                }
            } else if (r->kind == REF_CONTROL) {
                ct_frame_header control_header;
                int available =
                    ct_frame_available(r->peer->ctl.fd, CT_CONTROL_BUFFER_SIZE + CT_AEAD_TAG,
                                       CT_FLAG_ENCRYPTED, &control_header);
                if (available < 0)
                    peer_close(r->peer);
                else if (!available)
                    r->peer->ctl.read_retry_after_ms = ct_monotonic_ms() + 50;
                else {
                    r->peer->ctl.read_retry_after_ms = 0;
                    if (server_control(r->peer, cfg, &control_listeners, peers,
                                       CT_MAX_AUTH_CLIENTS))
                        peer_close(r->peer);
                }
            } else if (r->kind == REF_STARTING) {
                server_peer *p = r->peer;
                size_t index = 0;
                starting_conn *starting = find_starting(p, (ct_socket)(uintptr_t)r->ptr, &index);
                if (!starting)
                    continue;
                ct_frame_header ready;
                int available =
                    ct_plain_frame_available(starting->work.fd, CT_CONTROL_BUFFER_SIZE, &ready);
                if (available < 0 || (available > 0 && start_relay_finish(p, starting))) {
                    remove_starting(p, index, 1);
                } else if (available > 0) {
                    remove_starting(p, index, 0);
                } else {
                    starting->read_retry_after_ms = ct_monotonic_ms() + 50;
                }
            } else if (r->kind == REF_SERVICE) {
                server_peer *p = r->peer;
                server_service *s = (server_service *)r->ptr;
                ct_socket f = CT_INVALID_SOCKET;
                for (size_t i = 0; i < s->listen_n && f == CT_INVALID_SOCKET; i++)
                    f = ct_net_accept(s->listen_fd[i], NULL, 0);
                if (f != CT_INVALID_SOCKET) {
                    size_t streams = active_streams(p);
                    const char *reject_reason = NULL;
                    if (p->pending_n >= (size_t)cfg->max_pending_streams)
                        reject_reason = "PENDING_LIMIT";
                    else if (p->pending_n == MAX_PENDING)
                        reject_reason = "COMPILED_PENDING_LIMIT";
                    else if (streams >= (size_t)p->auth->max_streams)
                        reject_reason = "CLIENT_STREAM_LIMIT";
                    else if (streams >= (size_t)cfg->max_streams_per_client)
                        reject_reason = "SERVER_STREAM_LIMIT";
                    if (reject_reason) {
                        CT_LOGW("server",
                                "client_id=%s service_id=%s stream rejected reason=%s "
                                "pending=%zu/%d streams=%zu client_stream_limit=%d "
                                "server_stream_limit=%d",
                                p->ctl.client_id, s->cfg.id, reject_reason, p->pending_n,
                                cfg->max_pending_streams, streams, p->auth->max_streams,
                                cfg->max_streams_per_client);
                        ct_socket_close(f);
                    } else {
                        p->pending[p->pending_n++] =
                            (pending_conn){f, s, now + (uint64_t)cfg->connect_timeout * 1000u};
                        if (!p->work_n)
                            (void)ct_control_send(&p->ctl, CT_MSG_REQUEST_WORK_CONNECTION, 0, NULL,
                                                  0, 1000);
                        match_pending(p, cfg);
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
        for (size_t i = 0; i < incoming_n;) {
            if (incoming[i].deadline <= now)
                remove_incoming(incoming, &incoming_n, &incoming[i], 1);
            else
                i++;
        }
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
            for (size_t j = 0; j < p->starting_n;) {
                if (p->starting[j].deadline <= now)
                    remove_starting(p, j, 1);
                else
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
    while (incoming_n)
        remove_incoming(incoming, &incoming_n, &incoming[0], 1);
    listen_set_close(&control_listeners);
    free(rates);
    free(incoming);
    free(peers);
    free(refs);
    free(events);
    return 0;
}
