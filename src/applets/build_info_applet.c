#include "applets/applets.h"
#include "ctunnel/version.h"
#include "util/log.h"
#include <stdio.h>

#ifndef CTUNNEL_EVENT_NAME
#define CTUNNEL_EVENT_NAME "unknown"
#endif
#ifndef CTUNNEL_CONFIG_HASH
#define CTUNNEL_CONFIG_HASH "unknown"
#endif
#ifndef CTUNNEL_LINK_MODE
#define CTUNNEL_LINK_MODE "unknown"
#endif
#ifndef CTUNNEL_LIBC_NAME
#define CTUNNEL_LIBC_NAME "unknown"
#endif

static const char *role_name(void) {
#if defined(CONFIG_CTUNNEL_CLIENT) && defined(CONFIG_CTUNNEL_SERVER)
    return "client+server";
#elif defined(CONFIG_CTUNNEL_CLIENT)
    return "client-only";
#else
    return "server-only";
#endif
}

static const char *max_log_name(void) {
#ifdef CONFIG_LOG_TRACE
    return "trace";
#elif defined(CONFIG_LOG_DEBUG)
    return "debug";
#elif defined(CONFIG_LOG_INFO)
    return "info";
#elif defined(CONFIG_LOG_WARN)
    return "warn";
#else
    return "error";
#endif
}

#define CT_FEATURE_STATE(symbol)                                                                   \
    do {                                                                                           \
        puts(symbol ": on");                                                                       \
    } while (0)

#define CT_FEATURE_STATE_OFF(symbol)                                                               \
    do {                                                                                           \
        puts(symbol ": off");                                                                      \
    } while (0)

int ct_applet_build_info(int argc, char **argv) {
    (void)argc;
    (void)argv;
    ct_print_version();
    printf("event backend: %s\nconfiguration: %s\nrole: %s\nlink mode: %s\n"
           "C runtime: %s\nmaximum log level: %s\n",
           CTUNNEL_EVENT_NAME, CTUNNEL_CONFIG_HASH, role_name(), CTUNNEL_LINK_MODE,
           CTUNNEL_LIBC_NAME, max_log_name());
    puts("features:");
#ifdef CONFIG_FEATURE_IPV4
    CT_FEATURE_STATE("  IPv4");
#else
    CT_FEATURE_STATE_OFF("  IPv4");
#endif
#ifdef CONFIG_FEATURE_IPV6
    CT_FEATURE_STATE("  IPv6");
#else
    CT_FEATURE_STATE_OFF("  IPv6");
#endif
#ifdef CONFIG_FEATURE_TCP
    CT_FEATURE_STATE("  TCP forwarding");
#else
    CT_FEATURE_STATE_OFF("  TCP forwarding");
#endif
#ifdef CONFIG_FEATURE_UDP
    CT_FEATURE_STATE("  UDP forwarding");
#else
    CT_FEATURE_STATE_OFF("  UDP forwarding");
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL
    CT_FEATURE_STATE("  PROXY Protocol");
#else
    CT_FEATURE_STATE_OFF("  PROXY Protocol");
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL_V1
    CT_FEATURE_STATE("  PROXY Protocol v1");
#else
    CT_FEATURE_STATE_OFF("  PROXY Protocol v1");
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL_V2
    CT_FEATURE_STATE("  PROXY Protocol v2");
#else
    CT_FEATURE_STATE_OFF("  PROXY Protocol v2");
#endif
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
    CT_FEATURE_STATE("  data encryption");
#else
    CT_FEATURE_STATE_OFF("  data encryption");
#endif
#ifdef CONFIG_FEATURE_WORK_POOL
    CT_FEATURE_STATE("  work connection pool");
#else
    CT_FEATURE_STATE_OFF("  work connection pool");
#endif
#ifdef CONFIG_FEATURE_KEYGEN
    CT_FEATURE_STATE("  keygen");
#else
    CT_FEATURE_STATE_OFF("  keygen");
#endif
#ifdef CONFIG_FEATURE_FINGERPRINT
    CT_FEATURE_STATE("  fingerprint");
#else
    CT_FEATURE_STATE_OFF("  fingerprint");
#endif
    return 0;
}

int ct_applet_build_config(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("role=%s\nlink_mode=%s\nlibc=%s\nipv4=%s\nipv6=%s\n"
           "tcp=%s\nudp=%s\nproxy_protocol=%s\nproxy_protocol_v1=%s\n"
           "proxy_protocol_v2=%s\ndata_encryption=%s\nwork_pool=%s\nkeygen=%s\n"
           "fingerprint=%s\n"
           "event=%s\nlog_max=%s\nmax_clients=%d\nmax_services=%d\nmax_streams=%d\n"
           "max_pending=%d\nstream_buffer=%d\ncontrol_buffer=%d\nconfig_hash=%s\n",
           role_name(), CTUNNEL_LINK_MODE, CTUNNEL_LIBC_NAME,
#ifdef CONFIG_FEATURE_IPV4
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_IPV6
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_TCP
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_UDP
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL_V1
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL_V2
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_DATA_ENCRYPTION
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_WORK_POOL
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_KEYGEN
           "y",
#else
           "n",
#endif
#ifdef CONFIG_FEATURE_FINGERPRINT
           "y",
#else
           "n",
#endif
           CTUNNEL_EVENT_NAME, max_log_name(), CONFIG_MAX_CLIENTS, CONFIG_MAX_SERVICES,
           CONFIG_MAX_STREAMS, CONFIG_MAX_PENDING_STREAMS, CONFIG_STREAM_BUFFER_SIZE,
           CONFIG_CONTROL_BUFFER_SIZE, CTUNNEL_CONFIG_HASH);
    return 0;
}
