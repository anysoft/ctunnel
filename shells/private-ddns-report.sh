#!/bin/sh

# ============================================================
# ctunnel Private DDNS 上报脚本
# 运行环境：Buildroot / BusyBox ash
#
# 工作方式：
# 1. 读取指定 PPP 接口上的本地 IPv4、全局 IPv6。
# 2. 本地地址快照未变化时直接退出。
# 3. 本地地址变化后，分别通过 IPv4/IPv6 查询公网地址。
# 4. 对比上次成功上报的公网地址。
# 5. 任一公网地址变化时，只发送一次 PUT，
#    提交完整 IPv4/IPv6 快照。
# ============================================================

# -------------------- 必改配置 --------------------

# PPP 接口名称。
PPP_IFACE="ppp0"

# Worker 地址，不要以 / 结尾。
WORKER_BASE_URL="https://yourdomain.com"

CA_CERT_FILE="/usr/sbin/apps/etc/ssl/certs/ca-certificates.crt"
# 最终接口：
# /v1/ddns/report/home/router
DDNS_RECORD="home/router"

WRITE_TOKEN_FILE="/usr/sbin/apps/etc/cfregister/ddns-write-token"

# -------------------- 可选配置 --------------------

PUBLIC_IP_API="https://api.ip.sb/ip"

# 使用 /tmp 避免周期性写入 Flash。
# 设备重启后会重新上报一次。
STATE_DIR="/tmp/ctunnel-private-ddns"
LOCK_DIR="/tmp/ctunnel-private-ddns-report.lock"

CONNECT_TIMEOUT=10
MAX_TIME=20

# 1=输出日志，0=静默。
LOG_ENABLED=1

REPORT_URL="${WORKER_BASE_URL}/v1/ddns/report/${DDNS_RECORD}"

umask 077

log() {
    [ "$LOG_ENABLED" = "1" ] || return 0

    printf '%s %s\n' \
        "$(date '+%Y-%m-%d %H:%M:%S')" \
        "$*" >&2
}

cleanup() {
    rmdir "$LOCK_DIR" 2>/dev/null || true
}

read_state() {
    file="$1"

    if [ -f "$file" ]; then
        cat "$file" 2>/dev/null
    fi
}

write_state() {
    file="$1"
    value="$2"
    temp_file="${file}.tmp.$$"

    if ! printf '%s\n' "$value" > "$temp_file"; then
        rm -f "$temp_file"
        return 1
    fi

    if ! mv "$temp_file" "$file"; then
        rm -f "$temp_file"
        return 1
    fi
}

get_local_ipv4() {
    ip -4 addr show dev "$PPP_IFACE" 2>/dev/null |
        awk '
            /inet / {
                address = $2
                sub(/\/.*/, "", address)
                print address
                exit
            }
        '
}

get_local_ipv6() {
    ip -6 addr show dev "$PPP_IFACE" 2>/dev/null |
        awk '
            /inet6 / && /scope global/ {
                address = $2
                sub(/\/.*/, "", address)
                print address
                exit
            }
        '
}

is_valid_ipv4() {
    value="$1"

    [ -n "$value" ] || return 1

    old_ifs="$IFS"
    IFS='.'
    set -- $value
    IFS="$old_ifs"

    [ "$#" -eq 4 ] || return 1

    for octet in "$@"; do
        [ -n "$octet" ] || return 1

        case "$octet" in
            *[!0-9]*)
                return 1
                ;;
        esac

        [ "${#octet}" -le 3 ] || return 1

        if [ "$octet" -gt 255 ] 2>/dev/null; then
            return 1
        fi
    done

    return 0
}

is_valid_ipv6() {
    value="$1"

    [ -n "$value" ] || return 1

    case "$value" in
        *:*)
            ;;
        *)
            return 1
            ;;
    esac

    case "$value" in
        *[!0-9A-Fa-f:.]*)
            return 1
            ;;
    esac

    return 0
}

query_public_ipv4() {
    curl \
        -4 \
        --cacert "$CA_CERT_FILE" \
        --fail \
        --silent \
        --show-error \
        --connect-timeout "$CONNECT_TIMEOUT" \
        --max-time "$MAX_TIME" \
        "$PUBLIC_IP_API" |
        tr -d ' \t\r\n'
}

query_public_ipv6() {
    curl \
        -6 \
        --cacert "$CA_CERT_FILE" \
        --fail \
        --silent \
        --show-error \
        --connect-timeout "$CONNECT_TIMEOUT" \
        --max-time "$MAX_TIME" \
        "$PUBLIC_IP_API" |
        tr -d ' \t\r\n'
}

json_address_value() {
    value="$1"

    if [ -n "$value" ]; then
        printf '"%s"' "$value"
    else
        printf 'null'
    fi
}

report_snapshot() {
    token="$1"
    public_ipv4="$2"
    public_ipv6="$3"

    ipv4_json="$(
        json_address_value "$public_ipv4"
    )"

    ipv6_json="$(
        json_address_value "$public_ipv6"
    )"

    body="$(
        printf \
            '{"addresses":{"ipv4":%s,"ipv6":%s}}' \
            "$ipv4_json" \
            "$ipv6_json"
    )"

    curl \
        --cacert "$CA_CERT_FILE" \
        --interface "$PPP_IFACE" \
        --noproxy '*' \
        --fail \
        --silent \
        --show-error \
        --connect-timeout "$CONNECT_TIMEOUT" \
        --max-time "$MAX_TIME" \
        -X PUT \
        -H "Authorization: Bearer ${token}" \
        -H "Content-Type: application/json" \
        -H "Accept: application/json" \
        --data-binary "$body" \
        "$REPORT_URL"
}

main() {
    if ! ip link show >/dev/null 2>&1; then
        log "错误：ip 命令不可用"
        exit 1
    fi

    if ! curl --version >/dev/null 2>&1; then
        log "错误：curl 命令不可用"
        exit 1
    fi

    if ! awk 'BEGIN { exit 0 }' </dev/null >/dev/null 2>&1; then
        log "错误：awk 命令不可用"
        exit 1
    fi

    if [ ! -r "$WRITE_TOKEN_FILE" ]; then
        log "错误：无法读取 Token 文件：${WRITE_TOKEN_FILE}"
        exit 1
    fi

    token="$(
        tr -d '\r\n' < "$WRITE_TOKEN_FILE"
    )"

    if [ -z "$token" ]; then
        log "错误：WRITE_TOKEN 为空"
        exit 1
    fi

    if ! ip link show dev "$PPP_IFACE" >/dev/null 2>&1; then
        log "错误：接口不存在：${PPP_IFACE}"
        exit 1
    fi

    if ! mkdir -p "$STATE_DIR"; then
        log "错误：无法创建状态目录：${STATE_DIR}"
        exit 1
    fi

    # 防止 cron 重叠执行。
    if ! mkdir "$LOCK_DIR" 2>/dev/null; then
        exit 0
    fi

    trap cleanup 0 1 2 3 15

    local_ipv4="$(get_local_ipv4)"
    local_ipv6="$(get_local_ipv6)"

    if [ -z "$local_ipv4" ] &&
       [ -z "$local_ipv6" ]; then
        log "错误：接口 ${PPP_IFACE} 没有可用 IPv4 或全局 IPv6"
        exit 1
    fi

    local_ipv4_file="${STATE_DIR}/local_ipv4"
    local_ipv6_file="${STATE_DIR}/local_ipv6"
    public_ipv4_file="${STATE_DIR}/public_ipv4"
    public_ipv6_file="${STATE_DIR}/public_ipv6"

    previous_local_ipv4="$(
        read_state "$local_ipv4_file"
    )"

    previous_local_ipv6="$(
        read_state "$local_ipv6_file"
    )"

    if [ "$local_ipv4" = "$previous_local_ipv4" ] &&
       [ "$local_ipv6" = "$previous_local_ipv6" ]; then
        exit 0
    fi

    log "检测到接口地址变化"

    log "本地 IPv4：${previous_local_ipv4:-none} -> ${local_ipv4:-none}"
    log "本地 IPv6：${previous_local_ipv6:-none} -> ${local_ipv6:-none}"

    public_ipv4=""
    public_ipv6=""

    if [ -n "$local_ipv4" ]; then
        public_ipv4="$(
            query_public_ipv4
        )"

        if ! is_valid_ipv4 "$public_ipv4"; then
            log "错误：公网 IPv4 查询失败或返回无效地址：${public_ipv4:-empty}"
            exit 1
        fi
    fi

    if [ -n "$local_ipv6" ]; then
        public_ipv6="$(
            query_public_ipv6
        )"

        if ! is_valid_ipv6 "$public_ipv6"; then
            log "错误：公网 IPv6 查询失败或返回无效地址：${public_ipv6:-empty}"
            exit 1
        fi
    fi

    previous_public_ipv4="$(
        read_state "$public_ipv4_file"
    )"

    previous_public_ipv6="$(
        read_state "$public_ipv6_file"
    )"

    if [ "$public_ipv4" = "$previous_public_ipv4" ] &&
       [ "$public_ipv6" = "$previous_public_ipv6" ]; then

        # PPP 接口地址变化，但公网地址未变化。
        # 只更新本地检测状态，不占用一次 KV 写入。
        write_state \
            "$local_ipv4_file" \
            "$local_ipv4" ||
            exit 1

        write_state \
            "$local_ipv6_file" \
            "$local_ipv6" ||
            exit 1

        log "公网地址未变化，无需上报"
        exit 0
    fi

    log "公网 IPv4：${previous_public_ipv4:-none} -> ${public_ipv4:-none}"
    log "公网 IPv6：${previous_public_ipv6:-none} -> ${public_ipv6:-none}"

    response_file="${STATE_DIR}/last-report-response.tmp.$$"

    if ! report_snapshot \
        "$token" \
        "$public_ipv4" \
        "$public_ipv6" \
        > "$response_file"; then

        rm -f "$response_file"

        log "错误：向 Worker 上报失败；本地状态未更新，下次会重试"
        exit 1
    fi

    response="$(
        cat "$response_file" 2>/dev/null
    )"

    rm -f "$response_file"

    write_state \
        "$local_ipv4_file" \
        "$local_ipv4" ||
        exit 1

    write_state \
        "$local_ipv6_file" \
        "$local_ipv6" ||
        exit 1

    write_state \
        "$public_ipv4_file" \
        "$public_ipv4" ||
        exit 1

    write_state \
        "$public_ipv6_file" \
        "$public_ipv6" ||
        exit 1

    log "上报成功：${response}"
}

main "$@"
