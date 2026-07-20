#ifndef CT_LOG_H
#define CT_LOG_H
#include "generated/autoconf.h"
#include <stdint.h>
enum {
    CT_LOG_LEVEL_ERROR,
    CT_LOG_LEVEL_WARN,
    CT_LOG_LEVEL_INFO,
    CT_LOG_LEVEL_DEBUG,
    CT_LOG_LEVEL_TRACE
};
void ct_log_set_level(int level);
int ct_log_configure(const char *path, int rotate_days, int max_size_kib);
int ct_log_parse_level(const char *s);
const char *ct_log_level_name(int level);
void ct_log_write(int level, const char *module, const char *fmt, ...);
void ct_log_status(const char *module, const char *fmt, ...);
#ifdef CONFIG_LOG_ERROR
#define CT_LOG_ERROR(m, ...) ct_log_write(CT_LOG_LEVEL_ERROR, m, __VA_ARGS__)
#else
#define CT_LOG_ERROR(...) ((void)0)
#endif
#ifdef CONFIG_LOG_WARN
#define CT_LOG_WARN(m, ...) ct_log_write(CT_LOG_LEVEL_WARN, m, __VA_ARGS__)
#else
#define CT_LOG_WARN(...) ((void)0)
#endif
#ifdef CONFIG_LOG_INFO
#define CT_LOG_INFO(m, ...) ct_log_write(CT_LOG_LEVEL_INFO, m, __VA_ARGS__)
#else
#define CT_LOG_INFO(...) ((void)0)
#endif
#ifdef CONFIG_LOG_DEBUG
#define CT_LOG_DEBUG(m, ...) ct_log_write(CT_LOG_LEVEL_DEBUG, m, __VA_ARGS__)
#else
#define CT_LOG_DEBUG(...) ((void)0)
#endif
#ifdef CONFIG_LOG_TRACE
#define CT_LOG_TRACE(m, ...) ct_log_write(CT_LOG_LEVEL_TRACE, m, __VA_ARGS__)
#else
#define CT_LOG_TRACE(...) ((void)0)
#endif
#define CT_LOGE(...) CT_LOG_ERROR(__VA_ARGS__)
#define CT_LOGW(...) CT_LOG_WARN(__VA_ARGS__)
#define CT_LOGI(...) CT_LOG_INFO(__VA_ARGS__)
#define CT_LOGD(...) CT_LOG_DEBUG(__VA_ARGS__)
#define CT_LOGT(...) CT_LOG_TRACE(__VA_ARGS__)
#endif
