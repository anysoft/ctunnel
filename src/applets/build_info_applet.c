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

int ct_applet_build_info(int argc, char **argv) {
    (void)argc;
    (void)argv;
    ct_print_version();
    printf("event backend: %s\nconfiguration: %s\nrole: %s\nlink mode: %s\n"
           "C runtime: %s\nmaximum log level: %s\n",
           CTUNNEL_EVENT_NAME, CTUNNEL_CONFIG_HASH, role_name(), CTUNNEL_LINK_MODE,
           CTUNNEL_LIBC_NAME, max_log_name());
    return 0;
}

int ct_applet_build_config(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("role=%s\nlink_mode=%s\nlibc=%s\nipv4=%s\nipv6=%s\n"
           "data_encryption=%s\nwork_pool=%s\n"
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
           CTUNNEL_EVENT_NAME, max_log_name(), CONFIG_MAX_CLIENTS, CONFIG_MAX_SERVICES,
           CONFIG_MAX_STREAMS, CONFIG_MAX_PENDING_STREAMS, CONFIG_STREAM_BUFFER_SIZE,
           CONFIG_CONTROL_BUFFER_SIZE, CTUNNEL_CONFIG_HASH);
    return 0;
}
