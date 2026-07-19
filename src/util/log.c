#include "util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
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
static char g_log_path[CONFIG_MAX_PATH_LENGTH];
static int g_rotate_days = 7;
static int g_log_day = -1;

void ct_log_set_level(int l) {
    g_level = l <= CT_COMPILED_LOG_MAX ? l : CT_COMPILED_LOG_MAX;
}

int ct_log_configure(const char *path, int rotate_days) {
    g_log_path[0] = 0;
    g_log_day = -1;
    g_rotate_days = rotate_days < 0 ? 7 : rotate_days;
    if (!path || !*path || !strcmp(path, "-") || !strcmp(path, "stderr"))
        return 0;
    if (strlen(path) >= sizeof g_log_path)
        return -1;
    memcpy(g_log_path, path, strlen(path) + 1);
    FILE *file = fopen(g_log_path, "ab");
    if (!file) {
        g_log_path[0] = 0;
        return -1;
    }
    fclose(file);
    return 0;
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

static int day_number(const struct tm *tmv) {
    return (tmv->tm_year + 1900) * 10000 + (tmv->tm_mon + 1) * 100 + tmv->tm_mday;
}

static void suffix_for_day(char *out, size_t out_size, int day) {
    snprintf(out, out_size, "%04d%02d%02d", day / 10000, (day / 100) % 100, day % 100);
}

static int file_mtime_day(const char *path) {
#ifdef _WIN32
    (void)path;
    return -1;
#else
    struct stat st;
    if (stat(path, &st))
        return -1;
    struct tm tmv = {0};
    time_t t = st.st_mtime;
    localtime_r(&t, &tmv);
    return day_number(&tmv);
#endif
}

static void remove_old_logs(int today) {
    if (g_rotate_days <= 0)
        return;
    time_t now = time(NULL);
    for (int age = g_rotate_days; age < g_rotate_days + 32; age++) {
        time_t old = now - (time_t)age * 86400;
        struct tm tmv = {0};
#ifdef _WIN32
        localtime_s(&tmv, &old);
#else
        localtime_r(&old, &tmv);
#endif
        int day = day_number(&tmv);
        if (day >= today)
            continue;
        char suffix[16], old_path[sizeof g_log_path + 32];
        suffix_for_day(suffix, sizeof suffix, day);
        snprintf(old_path, sizeof old_path, "%s.%s", g_log_path, suffix);
        (void)remove(old_path);
    }
}

static void rotate_if_needed(int today) {
    if (!g_log_path[0])
        return;
    int current_day = g_log_day >= 0 ? g_log_day : file_mtime_day(g_log_path);
    if (current_day > 0 && current_day != today) {
        char suffix[16], rotated[sizeof g_log_path + 32];
        suffix_for_day(suffix, sizeof suffix, current_day);
        snprintf(rotated, sizeof rotated, "%s.%s", g_log_path, suffix);
        (void)remove(rotated);
        (void)rename(g_log_path, rotated);
    }
    g_log_day = today;
    remove_old_logs(today);
}

static void write_one(FILE *stream, const char *stamp, const char *level_name, const char *module,
                      const char *fmt, va_list ap) {
    fprintf(stream, "%s %-5s [%s] ", stamp, level_name, module);
    vfprintf(stream, fmt, ap);
    fputc('\n', stream);
    fflush(stream);
}

static void logv(const char *level_name, const char *module, const char *fmt, va_list ap) {
    time_t now = time(NULL);
    struct tm tmv = {0};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S%z", &tmv);
    va_list copy;
    va_copy(copy, ap);
    write_one(stderr, stamp, level_name, module, fmt, copy);
    va_end(copy);
    if (g_log_path[0]) {
        rotate_if_needed(day_number(&tmv));
        FILE *file = fopen(g_log_path, "ab");
        if (file) {
            write_one(file, stamp, level_name, module, fmt, ap);
            fclose(file);
        }
    }
}

void ct_log_write(int level, const char *module, const char *fmt, ...) {
    if (level > g_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    logv(ct_log_level_name(level), module, fmt, ap);
    va_end(ap);
}

void ct_log_status(const char *module, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv("info", module, fmt, ap);
    va_end(ap);
}
