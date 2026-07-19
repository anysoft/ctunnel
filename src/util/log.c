#include "util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef CONFIG_LOG_TRACE
#define CT_COMPILED_LOG_MAX CT_LOG_LEVEL_TRACE
#elif defined(CONFIG_LOG_DEBUG)
#define CT_COMPILED_LOG_MAX CT_LOG_LEVEL_DEBUG
#elif defined(CONFIG_LOG_INFO)
#define CT_COMPILED_LOG_MAX CT_LOG_LEVEL_INFO
#elif defined(CONFIG_LOG_WARN)
#define CT_COMPILED_LOG_MAX CT_LOG_LEVEL_WARN
#else
#define CT_COMPILED_LOG_MAX CT_LOG_LEVEL_ERROR
#endif
static int g_level = CT_COMPILED_LOG_MAX;
void ct_log_set_level(int l) {
    g_level = l <= CT_COMPILED_LOG_MAX ? l : CT_COMPILED_LOG_MAX;
}
const char *ct_log_level_name(int l) {
    switch (l) {
    case CT_LOG_LEVEL_ERROR:
        return "error";
#ifdef CONFIG_LOG_WARN
    case CT_LOG_LEVEL_WARN:
        return "warn";
#endif
#ifdef CONFIG_LOG_INFO
    case CT_LOG_LEVEL_INFO:
        return "info";
#endif
#ifdef CONFIG_LOG_DEBUG
    case CT_LOG_LEVEL_DEBUG:
        return "debug";
#endif
#ifdef CONFIG_LOG_TRACE
    case CT_LOG_LEVEL_TRACE:
        return "trace";
#endif
    default:
        return "unknown";
    }
}
int ct_log_parse_level(const char *s) {
    for (int i = 0; i <= CT_COMPILED_LOG_MAX; i++)
        if (strcmp(s, ct_log_level_name(i)) == 0)
            return i;
    return -1;
}
void ct_log_write(int level, const char *module, const char *fmt, ...) {
    if (level > g_level)
        return;
    time_t now = time(NULL);
    struct tm tmv = {0};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S%z", &tmv);
    fprintf(stderr, "%s %-5s [%s] ", stamp, ct_log_level_name(level), module);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
