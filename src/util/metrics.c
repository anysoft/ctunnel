#include "util/metrics.h"
#include "util/log.h"

static uint64_t g_metrics[CT_METRIC_COUNT];

static void update_peak(ct_metric_id current_id, ct_metric_id peak_id) {
    if (g_metrics[current_id] > g_metrics[peak_id])
        g_metrics[peak_id] = g_metrics[current_id];
}

void ct_metric_inc(ct_metric_id id) {
    ct_metric_add(id, 1);
}

void ct_metric_add(ct_metric_id id, uint64_t value) {
    if (id >= CT_METRIC_COUNT)
        return;
    uint64_t old = g_metrics[id];
    g_metrics[id] += value;
    if (g_metrics[id] < old)
        g_metrics[id] = UINT64_MAX;
    if (id == CT_METRIC_BUFFER_BYTES_CURRENT)
        update_peak(CT_METRIC_BUFFER_BYTES_CURRENT, CT_METRIC_BUFFER_BYTES_PEAK);
}

void ct_metric_sub(ct_metric_id id, uint64_t value) {
    if (id >= CT_METRIC_COUNT)
        return;
    g_metrics[id] = g_metrics[id] > value ? g_metrics[id] - value : 0;
}

void ct_metric_dec(ct_metric_id id) {
    ct_metric_sub(id, 1);
}

void ct_metric_set(ct_metric_id id, uint64_t value) {
    if (id >= CT_METRIC_COUNT)
        return;
    g_metrics[id] = value;
    if (id == CT_METRIC_BUFFER_BYTES_CURRENT)
        update_peak(CT_METRIC_BUFFER_BYTES_CURRENT, CT_METRIC_BUFFER_BYTES_PEAK);
}

uint64_t ct_metric_get(ct_metric_id id) {
    return id < CT_METRIC_COUNT ? g_metrics[id] : 0;
}

const char *ct_metric_name(ct_metric_id id) {
    static const char *names[CT_METRIC_COUNT] = {
        "connections_accepted_total",
        "connections_rejected_total",
        "active_streams",
        "pending_streams",
        "streams_opened_total",
        "streams_closed_total",
        "streams_failed_total",
        "work_idle",
        "work_active",
        "work_connect_failures_total",
        "control_reconnects_total",
        "bytes_c2s_total",
        "bytes_s2c_total",
        "buffer_bytes_current",
        "buffer_bytes_peak",
        "aead_failures_total",
        "auth_failures_total",
        "resource_limit_rejections_total",
        "udp_sessions_active",
        "udp_sessions_created_total",
        "udp_sessions_expired_total",
        "udp_sessions_rejected_total",
        "udp_datagrams_c2s_total",
        "udp_datagrams_s2c_total",
        "udp_bytes_c2s_total",
        "udp_bytes_s2c_total",
        "udp_datagrams_dropped_total",
        "udp_datagrams_oversized_total",
        "udp_replay_dropped_total",
        "udp_aead_failures_total",
        "udp_local_send_failures_total",
#ifdef CONFIG_FEATURE_PROXY_PROTOCOL
        "proxy_protocol_v1_connections_total",
        "proxy_protocol_v2_connections_total",
        "proxy_protocol_disabled_connections_total",
        "proxy_protocol_header_failures_total",
        "proxy_protocol_mode_mismatch_total",
        "proxy_protocol_local_write_failures_total",
#else
        "unused_metric",
        "unused_metric",
        "unused_metric",
        "unused_metric",
        "unused_metric",
        "unused_metric",
#endif
    };
    return id < CT_METRIC_COUNT ? names[id] : "unknown";
}

void ct_metrics_log_snapshot(const char *module) {
    for (int i = 0; i < CT_METRIC_COUNT; i++)
        ct_log_status(module, "metric %s=%llu", ct_metric_name((ct_metric_id)i),
                      (unsigned long long)g_metrics[i]);
}
