#include "ctunnel.h"
#include "ctunnel/crypto.h"
#include "util/log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static FILE *open_policy_file(const char *path) {
#ifdef _WIN32
    return fopen(path, "rb");
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 022)) {
        int saved_errno = errno ? errno : EPERM;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file)
        close(fd);
    return file;
#endif
}

static void defaults(ct_config *c) {
    memset(c, 0, sizeof *c);
#ifdef CONFIG_FEATURE_IPV6
    strcpy(c->bind_addr, "::");
#else
    strcpy(c->bind_addr, "0.0.0.0");
#endif
    c->bind_port = 7000;
    c->server_port = 7000;
    c->cipher_mask = 1u << CT_CIPHER_CHACHA;
    c->preferred_cipher = CT_CIPHER_CHACHA;
    c->heartbeat_interval = 20;
    c->heartbeat_timeout = 60;
    c->handshake_timeout = 10;
    c->connect_timeout = 5;
    c->reconnect_initial_delay = 1;
    c->reconnect_max_delay = 30;
    c->reconnect_jitter_percent = 20;
    c->pool_count = CONFIG_DEFAULT_POOL_COUNT;
    c->log_rotate_days = 7;
    c->log_max_size_kib = 1024;
    c->max_clients = CONFIG_MAX_CLIENTS;
    c->max_services_per_client = CONFIG_MAX_SERVICES;
    c->max_streams_per_client = CONFIG_DEFAULT_MAX_STREAMS;
    c->max_pending_streams = CONFIG_DEFAULT_MAX_PENDING_STREAMS;
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    c->default_data_encryption = CT_ENC_REQUIRED;
#else
    c->default_data_encryption = CT_ENC_DISABLED;
#endif
#ifdef CONFIG_LOG_TRACE
    c->log_level = CT_LOG_LEVEL_TRACE;
#elif defined(CONFIG_LOG_DEBUG)
    c->log_level = CT_LOG_LEVEL_DEBUG;
#elif defined(CONFIG_LOG_INFO)
    c->log_level = CT_LOG_LEVEL_INFO;
#elif defined(CONFIG_LOG_WARN)
    c->log_level = CT_LOG_LEVEL_WARN;
#else
    c->log_level = CT_LOG_LEVEL_ERROR;
#endif
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}
static int cp(char *d, size_t n, const char *s) {
    size_t z = strlen(s);
    if (z >= n)
        return -1;
    memcpy(d, s, z + 1);
    return 0;
}
static int key_seen(char seen[][32], size_t *count, size_t capacity, const char *key) {
    for (size_t i = 0; i < *count; i++)
        if (!strcmp(seen[i], key))
            return 1;
    if (*count == capacity || strlen(key) >= sizeof seen[0])
        return -1;
    memcpy(seen[(*count)++], key, strlen(key) + 1);
    return 0;
}
static int integer(const char *s, int lo, int hi, int *out) {
    char *e;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (errno || *e || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}
static int port(const char *s, uint16_t *out) {
    int v;
    if (integer(s, 1, 65535, &v))
        return -1;
    *out = (uint16_t)v;
    return 0;
}
static int address_family_unavailable(const char *address) {
#if !defined(CONFIG_FEATURE_IPV4) || !defined(CONFIG_FEATURE_IPV6)
    unsigned char storage[16];
#endif
    if (!address || !*address || !strcmp(address, "*"))
        return 0;
#ifndef CONFIG_FEATURE_IPV4
    if (inet_pton(AF_INET, address, storage) == 1)
        return 4;
#endif
#ifndef CONFIG_FEATURE_IPV6
    if (inet_pton(AF_INET6, address, storage) == 1)
        return 6;
#endif
    return 0;
}
static int enc(const char *s, ct_enc_mode *out) {
    if (!strcmp(s, "true") || !strcmp(s, "yes") || !strcmp(s, "on") || !strcmp(s, "enabled") ||
        !strcmp(s, "required")) {
#ifndef CONFIG_FEATURE_DATA_ENCRYPTION
        return -2;
#else
        *out = CT_ENC_REQUIRED;
#endif
    } else if (!strcmp(s, "false") || !strcmp(s, "no") || !strcmp(s, "off") ||
               !strcmp(s, "disabled"))
        *out = CT_ENC_DISABLED;
    else
        return -1;
    return 0;
}
static int ciphers(const char *s, unsigned *out) {
    char b[128];
    if (cp(b, sizeof b, s))
        return -1;
    unsigned m = 0;
    char *p = b;
    while (p) {
        char *tok = p, *comma = strchr(p, ',');
        if (comma) {
            *comma = 0;
            p = comma + 1;
        } else
            p = NULL;
        tok = trim(tok);
        if (!strcmp(tok, "xchacha20-poly1305"))
            m |= 1u << CT_CIPHER_CHACHA;
        else
            return -1;
    }
    if (!m)
        return -1;
    *out = m;
    return 0;
}
static int set_common(ct_config *c, const char *k, const char *v) {
    int x;
#define STR(name, field)                                                                           \
    if (!strcmp(k, name))                                                                          \
    return cp(c->field, sizeof c->field, v)
    STR("bind_addr", bind_addr);
    STR("server_addr", server_addr);
    STR("client_id", client_id);
    STR("identity_private_key", identity_private_key);
    STR("server_public_key", server_public_key);
    STR("authorized_clients_file", authorized_clients_file);
    STR("log_file", log_file);
#undef STR
    if (!strcmp(k, "mode")) {
        if (!strcmp(v, "server")) {
#ifndef CONFIG_CTUNNEL_SERVER
            return -2;
#else
            c->mode = CT_MODE_SERVER;
#endif
        } else if (!strcmp(v, "client")) {
#ifndef CONFIG_CTUNNEL_CLIENT
            return -2;
#else
            c->mode = CT_MODE_CLIENT;
#endif
        } else
            return -1;
        return 0;
    }
    if (!strcmp(k, "bind_port"))
        return port(v, &c->bind_port);
    if (!strcmp(k, "server_port"))
        return port(v, &c->server_port);
    if (!strcmp(k, "allowed_ciphers"))
        return ciphers(v, &c->cipher_mask);
    if (!strcmp(k, "preferred_cipher")) {
        if (!strcmp(v, "xchacha20-poly1305"))
            c->preferred_cipher = CT_CIPHER_CHACHA;
        else
            return -1;
        return 0;
    }
    if (!strcmp(k, "default_data_encryption"))
        return enc(v, &c->default_data_encryption);
    if (!strcmp(k, "log_level")) {
        x = ct_log_parse_level(v);
        if (x < 0)
            return -1;
        c->log_level = x;
        return 0;
    }
#define INT(name, field, lo, hi)                                                                   \
    if (!strcmp(k, name)) {                                                                        \
        return integer(v, lo, hi, &c->field);                                                      \
    }
    INT("heartbeat_interval", heartbeat_interval, 1, 3600);
    INT("heartbeat_timeout", heartbeat_timeout, 2, 7200);
    INT("handshake_timeout", handshake_timeout, 1, 120);
    INT("connect_timeout", connect_timeout, 1, 120);
    INT("reconnect_initial_delay", reconnect_initial_delay, 1, 3600);
    INT("reconnect_max_delay", reconnect_max_delay, 1, 86400);
    INT("reconnect_jitter_percent", reconnect_jitter_percent, 0, 90);
#ifdef CONFIG_FEATURE_WORK_POOL
    INT("pool_count", pool_count, 0, CONFIG_MAX_STREAMS);
#else
    if (!strcmp(k, "pool_count"))
        return !strcmp(v, "0") ? 0 : -2;
#endif
    INT("max_clients", max_clients, 1, CONFIG_MAX_CLIENTS);
    INT("max_services_per_client", max_services_per_client, 1, CT_MAX_SERVICES);
    INT("max_streams_per_client", max_streams_per_client, 1, CONFIG_MAX_STREAMS);
    INT("max_pending_streams", max_pending_streams, 1, CONFIG_MAX_PENDING_STREAMS);
    INT("log_rotate_days", log_rotate_days, 0, 3650);
    INT("log_max_size_kib", log_max_size_kib, 64, 1048576);
#undef INT
    if (!strcmp(k, "control_encryption"))
        return strcmp(v, "required") ? -1 : 0;
    if (!strcmp(k, "auth_mode"))
        return strcmp(v, "public-key") ? -1 : 0;
    return 1;
}

static int set_default_log_file(ct_config *c) {
    if (c->log_file[0])
        return 0;
    const char *name = "ctunnel.log";
#ifdef CONFIG_CTUNNEL_CLIENT
    if (c->mode == CT_MODE_CLIENT)
        name = "ctunnel-client.log";
#endif
#ifdef CONFIG_CTUNNEL_SERVER
    if (c->mode == CT_MODE_SERVER)
        name = "ctunnel-server.log";
#endif
    const char *last_slash = strrchr(c->config_path, '/');
#ifdef _WIN32
    const char *last_backslash = strrchr(c->config_path, '\\');
    if (!last_slash || (last_backslash && last_backslash > last_slash))
        last_slash = last_backslash;
#endif
    if (!last_slash)
        return cp(c->log_file, sizeof c->log_file, name);
    size_t dir_len = (size_t)(last_slash - c->config_path + 1);
    size_t name_len = strlen(name);
    if (dir_len + name_len >= sizeof c->log_file)
        return -1;
    memcpy(c->log_file, c->config_path, dir_len);
    memcpy(c->log_file + dir_len, name, name_len + 1);
    return 0;
}

static int set_service(ct_service_config *s, const char *k, const char *v) {
    if (!strcmp(k, "type")) {
        if (!strcmp(v, "tcp"))
            s->type = 1;
#ifdef CONFIG_FEATURE_UDP
        else if (!strcmp(v, "udp"))
            s->type = 2;
#else
        else if (!strcmp(v, "udp"))
            return -2;
#endif
        else if (!strcmp(v, "http"))
            s->type = 3;
        else if (!strcmp(v, "https"))
            s->type = 4;
        else
            return -1;
        return 0;
    }
    if (!strcmp(k, "remote_addr"))
        return cp(s->remote_addr, sizeof s->remote_addr, v);
    if (!strcmp(k, "local_addr"))
        return cp(s->local_addr, sizeof s->local_addr, v);
    if (!strcmp(k, "remote_port"))
        return port(v, &s->remote_port);
    if (!strcmp(k, "local_port"))
        return port(v, &s->local_port);
    if (!strcmp(k, "data_encryption"))
        return enc(v, &s->encryption);
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL
    if (!strcmp(k, "proxy_protocol")) {
        if (!strcmp(v, "off"))
            s->proxy_protocol = CT_PROXY_PROTOCOL_OFF;
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL_V1
        else if (!strcmp(v, "v1"))
            s->proxy_protocol = CT_PROXY_PROTOCOL_V1;
#else
        else if (!strcmp(v, "v1"))
            return -2;
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL_V2
        else if (!strcmp(v, "v2"))
            s->proxy_protocol = CT_PROXY_PROTOCOL_V2;
#else
        else if (!strcmp(v, "v2"))
            return -2;
#endif
        else
            return -1;
        return 0;
    }
    if (!strcmp(k, "proxy_protocol_tlv"))
        return !strcmp(v, "off") ? 0 : -1;
#endif
#ifdef CONFIG_FEATURE_UDP
    if (!strcmp(k, "udp_idle_timeout"))
        return (s->udp_options_seen = 1, integer(v, 1, 86400, &s->udp_idle_timeout));
    if (!strcmp(k, "udp_reply_timeout"))
        return (s->udp_options_seen = 1, integer(v, 1, 86400, &s->udp_reply_timeout));
    if (!strcmp(k, "udp_max_sessions"))
        return (s->udp_options_seen = 1,
                integer(v, 1, CONFIG_MAX_UDP_SESSIONS, &s->udp_max_sessions));
    if (!strcmp(k, "udp_max_datagram_size"))
        return (s->udp_options_seen = 1,
                integer(v, 512, CONFIG_MAX_UDP_DATAGRAM_SIZE, &s->udp_max_datagram_size));
#else
    if (!strcmp(k, "udp_idle_timeout") || !strcmp(k, "udp_reply_timeout") ||
        !strcmp(k, "udp_max_sessions") || !strcmp(k, "udp_max_datagram_size"))
        return -2;
#endif
    return 1;
}
int ct_config_load(const char *path, ct_config *c, char *err, size_t en) {
    FILE *f = open_policy_file(path);
    if (!f) {
        snprintf(err, en,
                 "cannot securely open %s (must be a regular non-symlink file and not "
                 "group/world writable): %s",
                 path, strerror(errno));
        return -1;
    }
    defaults(c);
    cp(c->config_path, sizeof c->config_path, path);
    char line[CT_MAX_PATH * 2], section[CT_MAX_SERVICE_ID + 1] = "";
    char common_seen[40][32] = {{0}}, section_seen[16][32] = {{0}};
    size_t common_seen_count = 0, section_seen_count = 0;
    ct_service_config *svc = NULL;
    unsigned ln = 0;
    int rc = -1;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(err, en, "line %u too long", ln);
            goto out;
        }
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';')
            continue;
        if (*p == '[') {
            char *q = strchr(p, ']');
            if (!q || q[1]) {
                snprintf(err, en, "line %u: invalid section", ln);
                goto out;
            }
            *q = 0;
            if (cp(section, sizeof section, p + 1)) {
                snprintf(err, en, "line %u: section too long", ln);
                goto out;
            }
            if (!strcmp(section, "common")) {
                svc = NULL;
            } else {
                if (c->service_count == CT_MAX_SERVICES) {
                    snprintf(err, en, "too many services");
                    goto out;
                }
                svc = &c->services[c->service_count++];
                memset(svc, 0, sizeof *svc);
                cp(svc->id, sizeof svc->id, section);
#ifdef CONFIG_FEATURE_IPV6
                strcpy(svc->remote_addr, "::");
#else
                strcpy(svc->remote_addr, "0.0.0.0");
#endif
#ifdef CONFIG_FEATURE_IPV4
                strcpy(svc->local_addr, "127.0.0.1");
#else
                strcpy(svc->local_addr, "::1");
#endif
                svc->encryption = c->default_data_encryption;
#ifdef CONFIG_FEATURE_UDP
                svc->udp_idle_timeout = 60;
                svc->udp_reply_timeout = 5;
                svc->udp_max_sessions = CONFIG_MAX_UDP_SESSIONS;
                svc->udp_max_datagram_size = CONFIG_MAX_UDP_DATAGRAM_SIZE;
#endif
            }
            memset(section_seen, 0, sizeof section_seen);
            section_seen_count = 0;
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq || !section[0]) {
            snprintf(err, en, "line %u: expected key=value", ln);
            goto out;
        }
        *eq = 0;
        char *k = trim(p), *v = trim(eq + 1);
        int duplicate = svc ? key_seen(section_seen, &section_seen_count,
                                       sizeof section_seen / sizeof section_seen[0], k)
                            : key_seen(common_seen, &common_seen_count,
                                       sizeof common_seen / sizeof common_seen[0], k);
        if (duplicate) {
            snprintf(err, en, "line %u: %s key %s", ln, duplicate > 0 ? "duplicate" : "too many",
                     k);
            goto out;
        }
        int z = svc ? set_service(svc, k, v) : set_common(c, k, v);
        if (z) {
            snprintf(err, en, "line %u: %s %s '%s'", ln,
                     z > 0 ? "unknown key"
                           : (z == -2 ? "feature unavailable for" : "invalid value for"),
                     k, v);
            goto out;
        }
    }
    if (ferror(f)) {
        snprintf(err, en, "read error");
        goto out;
    }
#ifdef CONFIG_FEATURE_ENV_OVERRIDE
    const char *environment_value = getenv("CTUNNEL_LOG_LEVEL");
    if (environment_value) {
        int level = ct_log_parse_level(environment_value);
        if (level < 0) {
            snprintf(err, en, "CTUNNEL_LOG_LEVEL is unavailable in this build");
            goto out;
        }
        c->log_level = level;
    }
    environment_value = getenv("CTUNNEL_POOL_COUNT");
    if (environment_value && integer(environment_value, 0, CONFIG_MAX_STREAMS, &c->pool_count)) {
        snprintf(err, en, "CTUNNEL_POOL_COUNT exceeds the compiled limit");
        goto out;
    }
#endif
    if (set_default_log_file(c)) {
        snprintf(err, en, "default log file path is too long");
        goto out;
    }
    rc = ct_config_validate(c, err, en);
out:
    fclose(f);
    return rc;
}
int ct_config_validate(const ct_config *c, char *e, size_t n) {
    if (c->mode == CT_MODE_NONE) {
        snprintf(e, n, "common.mode is required");
        return -1;
    }
#ifndef CONFIG_CTUNNEL_SERVER
    if (c->mode == CT_MODE_SERVER) {
        snprintf(e, n, "server role is not compiled into this binary");
        return -1;
    }
#endif
#ifndef CONFIG_CTUNNEL_CLIENT
    if (c->mode == CT_MODE_CLIENT) {
        snprintf(e, n, "client role is not compiled into this binary");
        return -1;
    }
#endif
    if (!c->identity_private_key[0]) {
        snprintf(e, n, "identity_private_key is required");
        return -1;
    }
    if (!(c->cipher_mask & (1u << c->preferred_cipher))) {
        snprintf(e, n, "preferred_cipher is not allowed");
        return -1;
    }
    int unavailable_family = address_family_unavailable(c->bind_addr);
    if (!unavailable_family && c->mode == CT_MODE_CLIENT)
        unavailable_family = address_family_unavailable(c->server_addr);
    if (unavailable_family) {
        snprintf(e, n, "IPv%d address used but IPv%d is not compiled in", unavailable_family,
                 unavailable_family);
        return -1;
    }
    if (c->heartbeat_timeout <= c->heartbeat_interval) {
        snprintf(e, n, "heartbeat_timeout must exceed interval");
        return -1;
    }
    if (c->max_clients > CT_MAX_AUTH_CLIENTS) {
        snprintf(e, n, "max_clients exceeds compiled limit %d", CT_MAX_AUTH_CLIENTS);
        return -1;
    }
    if (c->max_services_per_client > CONFIG_MAX_SERVICES ||
        c->max_streams_per_client > CONFIG_MAX_STREAMS ||
        c->max_pending_streams > CONFIG_MAX_PENDING_STREAMS) {
        snprintf(e, n, "runtime resource limit exceeds a compiled hard limit");
        return -1;
    }
#ifndef CONFIG_FEATURE_WORK_POOL
    if (c->pool_count != 0) {
        snprintf(e, n, "work pool is not compiled into this binary");
        return -1;
    }
#endif
#ifndef CONFIG_FEATURE_DATA_ENCRYPTION
    if (c->default_data_encryption != CT_ENC_DISABLED) {
        snprintf(e, n, "relay data encryption is not compiled into this binary");
        return -1;
    }
#endif
    if (c->mode == CT_MODE_SERVER) {
        if (!c->authorized_clients_file[0]) {
            snprintf(e, n, "authorized_clients_file is required");
            return -1;
        }
        if (c->service_count) {
            snprintf(e, n, "server config cannot contain service sections");
            return -1;
        }
    } else {
        if (!c->server_addr[0] || !c->client_id[0] || !c->server_public_key[0]) {
            snprintf(e, n, "client server_addr, client_id and server_public_key are required");
            return -1;
        }
        for (size_t i = 0; i < c->service_count; i++) {
            const ct_service_config *s = &c->services[i];
            unavailable_family = address_family_unavailable(s->remote_addr);
            if (!unavailable_family)
                unavailable_family = address_family_unavailable(s->local_addr);
            if (unavailable_family) {
                snprintf(e, n, "service %s: IPv%d is not compiled in", s->id, unavailable_family);
                return -1;
            }
            if (s->type != 1 && s->type != 2) {
                snprintf(e, n, "service %s: unsupported service type", s->id);
                return -1;
            }
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL
            if (s->type != 1 && s->proxy_protocol != CT_PROXY_PROTOCOL_OFF) {
                snprintf(e, n, "service %s: PROXY Protocol is only supported for TCP", s->id);
                return -1;
            }
#endif
#ifndef CONFIG_FEATURE_UDP
            if (s->type == 2) {
                snprintf(e, n, "service %s: UDP is not compiled into this binary", s->id);
                return -1;
            }
#endif
#ifdef CONFIG_FEATURE_UDP
            if (s->type == 1 && s->udp_options_seen) {
                snprintf(e, n, "service %s: UDP-only option used with TCP service", s->id);
                return -1;
            }
#endif
#ifndef CONFIG_FEATURE_DATA_ENCRYPTION
            if (s->encryption != CT_ENC_DISABLED) {
                snprintf(e, n, "service %s: data encryption is unavailable in this build", s->id);
                return -1;
            }
#endif
            if (!s->remote_port || !s->local_port) {
                snprintf(e, n, "service %s: ports are required", s->id);
                return -1;
            }
            for (size_t j = 0; j < i; j++)
                if (!strcmp(s->id, c->services[j].id)) {
                    snprintf(e, n, "duplicate service %s", s->id);
                    return -1;
                }
        }
    }
    return 0;
}

int ct_config_validate_security_files(const ct_config *c, char *e, size_t n) {
#ifndef _WIN32
    struct stat st;
    if (lstat(c->identity_private_key, &st) != 0) {
        snprintf(e, n, "cannot stat identity_private_key: %s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        snprintf(e, n, "identity_private_key must be a regular non-symlink file");
        return -1;
    }
    if ((st.st_mode & 077) != 0) {
        snprintf(e, n, "identity_private_key permissions must be 0600 or stricter");
        return -1;
    }
    if (c->mode == CT_MODE_SERVER) {
        if (lstat(c->authorized_clients_file, &st) != 0 || !S_ISREG(st.st_mode) ||
            S_ISLNK(st.st_mode) || (st.st_mode & 022) != 0) {
            snprintf(e, n,
                     "authorized_clients_file must be a regular non-symlink file and not "
                     "group/world writable");
            return -1;
        }
    }
#else
    (void)c;
    (void)e;
    (void)n;
#endif
    uint8_t private_key[CT_ED_SECRET];
    if (ct_load_private_key(c->identity_private_key, private_key)) {
        snprintf(e, n, "identity_private_key has invalid type, permissions, or encoding");
        return -1;
    }
    ct_crypto_wipe(private_key, sizeof private_key);
    if (c->mode == CT_MODE_CLIENT) {
        uint8_t public_key[CT_ED_PUBLIC];
        if (ct_load_public_key(c->server_public_key, public_key)) {
            snprintf(e, n, "server_public_key has invalid type or encoding");
            return -1;
        }
    } else {
        for (size_t i = 0; i < c->client_count; i++) {
            uint8_t public_key[CT_ED_PUBLIC];
            if (ct_load_public_key(c->clients[i].public_key, public_key)) {
                snprintf(e, n, "authorized client %s public key is invalid", c->clients[i].id);
                return -1;
            }
        }
    }
    return 0;
}
static int parse_range(const char *v, ct_port_range *r) {
    char *b;
    long a = strtol(v, &b, 10), z = a;
    if (*b == '-')
        z = strtol(b + 1, &b, 10);
    if (*b || a < 1 || z < a || z > 65535)
        return -1;
    r->first = (uint16_t)a;
    r->last = (uint16_t)z;
    return 0;
}
int ct_authorized_load(const char *path, ct_config *c, char *err, size_t en) {
    FILE *f = open_policy_file(path);
    if (!f) {
        snprintf(err, en, "cannot securely open authorized clients %s: %s", path, strerror(errno));
        return -1;
    }
    char line[CT_MAX_PATH * 2];
    ct_authorized_client *a = NULL;
    char section_seen[4][32] = {{0}};
    size_t section_seen_count = 0;
    unsigned ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(err, en, "clients line %u too long", ln);
            goto bad;
        }
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';')
            continue;
        if (*p == '[') {
            char *q = strchr(p, ']');
            if (!q || q[1] || strncmp(p, "[client.", 8)) {
                snprintf(err, en, "clients line %u: expected [client.ID]", ln);
                goto bad;
            }
            *q = 0;
            if (c->client_count == CT_MAX_AUTH_CLIENTS) {
                snprintf(err, en, "too many authorized clients");
                goto bad;
            }
            for (size_t i = 0; i < c->client_count; i++)
                if (!strcmp(c->clients[i].id, p + 8)) {
                    snprintf(err, en, "duplicate authorized client %s", p + 8);
                    goto bad;
                }
            a = &c->clients[c->client_count++];
            memset(a, 0, sizeof *a);
            if (cp(a->id, sizeof a->id, p + 8)) {
                snprintf(err, en, "client id too long");
                goto bad;
            }
#ifdef CONFIG_FEATURE_IPV6
            strcpy(a->allow_addr, "::");
#else
            strcpy(a->allow_addr, "0.0.0.0");
#endif
            a->max_services = c->max_services_per_client;
            a->max_streams = c->max_streams_per_client;
            memset(section_seen, 0, sizeof section_seen);
            section_seen_count = 0;
            continue;
        }
        char *q = strchr(p, '=');
        if (!q || !a) {
            snprintf(err, en, "clients line %u malformed", ln);
            goto bad;
        }
        *q = 0;
        char *k = trim(p), *v = trim(q + 1);
        if (strcmp(k, "allow_remote_port")) {
            int duplicate = key_seen(section_seen, &section_seen_count,
                                     sizeof section_seen / sizeof section_seen[0], k);
            if (duplicate) {
                snprintf(err, en, "clients line %u: %s key %s", ln,
                         duplicate > 0 ? "duplicate" : "too many", k);
                goto bad;
            }
        }
        int badv = 0;
        if (!strcmp(k, "public_key"))
            badv = cp(a->public_key, sizeof a->public_key, v);
        else if (!strcmp(k, "allow_bind_addr"))
            badv = cp(a->allow_addr, sizeof a->allow_addr, v);
        else if (!strcmp(k, "allow_remote_port")) {
            if (a->port_count == CT_MAX_PORT_RANGES || parse_range(v, &a->ports[a->port_count]))
                badv = -1;
            else
                a->port_count++;
        } else if (!strcmp(k, "max_services"))
            badv = integer(v, 1, CT_MAX_SERVICES, &a->max_services);
        else if (!strcmp(k, "max_streams"))
            badv = integer(v, 1, CONFIG_MAX_STREAMS, &a->max_streams);
        else {
            snprintf(err, en, "clients line %u unknown key %s", ln, k);
            goto bad;
        }
        if (badv) {
            snprintf(err, en, "clients line %u invalid %s", ln, k);
            goto bad;
        }
    }
    fclose(f);
    for (size_t i = 0; i < c->client_count; i++)
        if (address_family_unavailable(c->clients[i].allow_addr)) {
            snprintf(err, en, "client %s uses an address family not compiled in", c->clients[i].id);
            return -1;
        } else if (!c->clients[i].public_key[0] || !c->clients[i].port_count) {
            snprintf(err, en, "client %s requires public_key and allow_remote_port",
                     c->clients[i].id);
            return -1;
        }
    return 0;
bad:
    fclose(f);
    return -1;
}
const ct_authorized_client *ct_authorized_find(const ct_config *c, const char *id) {
    for (size_t i = 0; i < c->client_count; i++)
        if (!strcmp(c->clients[i].id, id))
            return &c->clients[i];
    return NULL;
}
bool ct_authorized_port(const ct_authorized_client *a, const char *addr, uint16_t p) {
    if (strcmp(a->allow_addr, "*") && strcmp(a->allow_addr, addr))
        return false;
    for (size_t i = 0; i < a->port_count; i++)
        if (p >= a->ports[i].first && p <= a->ports[i].last)
            return true;
    return false;
}
