#!/usr/bin/env bash

set -u
set -o pipefail

# ============================================================
# ctunnel Private DDNS 解析与配置更新脚本
# 运行环境：Linux + Bash + systemd
#
# 工作方式：
# 1. 读取 client.ini 的 [common] server_addr/server_port。
# 2. 检查到该地址和端口的 ESTABLISHED TCP 连接数。
# 3. 连接持续为 0 达到阈值后，从 Worker 获取最新地址。
# 4. 地址变化时备份并更新 client.ini。
# 5. 可选执行 ctunnel configtest。
# 6. 重启 ctunnel-client.service；地址未变化但连接仍为 0 时也重启。
# ============================================================

# -------------------- 必改配置 --------------------
WORKER_BASE_URL="${WORKER_BASE_URL:-https://yourdomain.com}"
DDNS_RECORD="${DDNS_RECORD:-home/router}"
READ_TOKEN_FILE="${READ_TOKEN_FILE:-/etc/ctunnel/secrets/ddns-read-token}"
CLIENT_INI="${CLIENT_INI:-/etc/ctunnel/client.ini}"
SERVICE_NAME="${SERVICE_NAME:-ctunnel-client.service}"
CTUNNEL_BIN="${CTUNNEL_BIN:-/usr/local/bin/ctunnel}"

# ipv4、ipv6 或 auto；auto 由 Worker 优先返回 IPv6。
RESOLVE_FAMILY="${RESOLVE_FAMILY:-ipv6}"

# -------------------- 可选配置 --------------------
NO_CONNECTION_GRACE_SECONDS="${NO_CONNECTION_GRACE_SECONDS:-60}"
MIN_RESOLVE_INTERVAL_SECONDS="${MIN_RESOLVE_INTERVAL_SECONDS:-60}"
CONNECT_TIMEOUT="${CONNECT_TIMEOUT:-10}"
MAX_TIME="${MAX_TIME:-20}"
ENABLE_CONFIGTEST="${ENABLE_CONFIGTEST:-1}"
LOG_ENABLED="${LOG_ENABLED:-1}"

STATE_DIR="${STATE_DIR:-/run/ctunnel-private-ddns}"
LOCK_DIR="${LOCK_DIR:-/run/ctunnel-private-ddns-resolve.lock}"

RESOLVE_URL="${WORKER_BASE_URL}/v1/ddns/resolve/${DDNS_RECORD}?format=text&family=${RESOLVE_FAMILY}"
NO_CONNECTION_SINCE_FILE="${STATE_DIR}/no-connection-since"
LAST_RESOLVE_AT_FILE="${STATE_DIR}/last-resolve-at"

umask 077

log() {
    [[ "$LOG_ENABLED" == "1" ]] || return 0
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >&2
}

cleanup_lock() {
    rm -rf "$LOCK_DIR" 2>/dev/null || true
}

acquire_lock() {
    local old_pid=""

    if mkdir "$LOCK_DIR" 2>/dev/null; then
        printf '%s\n' "$$" > "${LOCK_DIR}/pid"
        trap cleanup_lock EXIT INT TERM
        return 0
    fi

    if [[ -r "${LOCK_DIR}/pid" ]]; then
        old_pid="$(cat "${LOCK_DIR}/pid" 2>/dev/null || true)"
    fi

    if [[ "$old_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_pid" 2>/dev/null; then
        return 1
    fi

    rm -rf "$LOCK_DIR" 2>/dev/null || return 1
    mkdir "$LOCK_DIR" 2>/dev/null || return 1
    printf '%s\n' "$$" > "${LOCK_DIR}/pid"
    trap cleanup_lock EXIT INT TERM
    return 0
}

read_file_value() {
    local file="$1"
    [[ -f "$file" ]] && cat "$file" 2>/dev/null || true
}

write_file_value() {
    local file="$1"
    local value="$2"
    local temp_file="${file}.tmp.$$"

    printf '%s\n' "$value" > "$temp_file" || {
        rm -f "$temp_file"
        return 1
    }

    mv "$temp_file" "$file"
}

trim_shell_value() {
    local value="$1"

    value="${value#${value%%[![:space:]]*}}"
    value="${value%${value##*[![:space:]]}}"

    if [[ ${#value} -ge 2 ]]; then
        if [[ "${value:0:1}" == '"' && "${value: -1}" == '"' ]]; then
            value="${value:1:${#value}-2}"
        elif [[ "${value:0:1}" == "'" && "${value: -1}" == "'" ]]; then
            value="${value:1:${#value}-2}"
        fi
    fi

    printf '%s\n' "$value"
}

# 读取 [common] 下的指定键。awk 写法保持 POSIX/mawk 兼容。
ini_get_common_value() {
    local key="$1"
    local file="$2"
    local value

    value="$(awk -v wanted="$key" '
        BEGIN { section = "" }
        {
            line = $0
            sub(/\r$/, "", line)

            if (line ~ /^[[:space:]]*[#;]/) next

            if (line ~ /^[[:space:]]*\[[^]]+\][[:space:]]*$/) {
                section = line
                sub(/^[[:space:]]*\[/, "", section)
                sub(/\][[:space:]]*$/, "", section)
                gsub(/^[[:space:]]+/, "", section)
                gsub(/[[:space:]]+$/, "", section)
                next
            }

            if (section != "common") next

            eq = index(line, "=")
            if (eq == 0) next

            lhs = substr(line, 1, eq - 1)
            rhs = substr(line, eq + 1)
            gsub(/^[[:space:]]+/, "", lhs)
            gsub(/[[:space:]]+$/, "", lhs)

            if (lhs != wanted) next

            sub(/[[:space:]]*[#;].*$/, "", rhs)
            gsub(/^[[:space:]]+/, "", rhs)
            gsub(/[[:space:]]+$/, "", rhs)
            print rhs
            exit
        }
    ' "$file")"

    trim_shell_value "$value"
}

normalize_endpoint_host() {
    local value="$1"
    value="${value#[}"
    value="${value%]}"
    value="${value%%\%*}"
    printf '%s\n' "${value,,}"
}

is_ipv4_literal() {
    local value="$1"
    local old_ifs="$IFS"
    local octet

    [[ "$value" == *.* && "$value" != *:* ]] || return 1

    IFS='.'
    set -- $value
    IFS="$old_ifs"

    [[ $# -eq 4 ]] || return 1

    for octet in "$@"; do
        [[ "$octet" =~ ^[0-9]+$ ]] || return 1
        [[ ${#octet} -le 3 ]] || return 1
        (( 10#$octet >= 0 && 10#$octet <= 255 )) || return 1
    done

    return 0
}

# 返回目标地址与端口的 ESTABLISHED 连接数。
count_established_connections() {
    local target_addr="$1"
    local target_port="$2"
    local normalized_addr
    local output
    local pattern
    local count

    normalized_addr="$(normalize_endpoint_host "$target_addr")"

    if command -v ss >/dev/null 2>&1; then
        output="$(ss -H -tn state established 2>/dev/null || true)"

        if is_ipv4_literal "$normalized_addr"; then
            pattern="${normalized_addr}:${target_port}"
        else
            pattern="[${normalized_addr}]:${target_port}"
        fi

        count="$(printf '%s\n' "$output" | grep -Fic -- "$pattern" || true)"

        # 少数 ss 版本显示 IPv6 时不带方括号。
        if [[ "$count" == "0" && "$normalized_addr" == *:* ]]; then
            pattern="${normalized_addr}:${target_port}"
            count="$(printf '%s\n' "$output" | grep -Fic -- "$pattern" || true)"
        fi

        printf '%s\n' "$count"
        return 0
    fi

    if command -v netstat >/dev/null 2>&1; then
        output="$(netstat -tn 2>/dev/null | grep 'ESTABLISHED' || true)"
        pattern="${normalized_addr}:${target_port}"
        count="$(printf '%s\n' "$output" | grep -Fic -- "$pattern" || true)"
        printf '%s\n' "$count"
        return 0
    fi

    log "错误：找不到 ss 或 netstat，无法统计连接"
    return 1
}

validate_ip_literal() {
    local value="$1"

    [[ -n "$value" ]] || return 1
    [[ "$value" =~ ^[0-9A-Fa-f:.]+$ ]] || return 1

    if command -v python3 >/dev/null 2>&1; then
        python3 - "$value" "$RESOLVE_FAMILY" <<'PY'
import ipaddress
import sys

try:
    address = ipaddress.ip_address(sys.argv[1])
except ValueError:
    raise SystemExit(1)

family = sys.argv[2]
if family == "ipv4" and address.version != 4:
    raise SystemExit(1)
if family == "ipv6" and address.version != 6:
    raise SystemExit(1)
PY
        return $?
    fi

    case "$RESOLVE_FAMILY" in
        ipv4) is_ipv4_literal "$value" ;;
        ipv6) [[ "$value" == *:* ]] ;;
        auto)  is_ipv4_literal "$value" || [[ "$value" == *:* ]] ;;
        *)     return 1 ;;
    esac
}

resolve_address() {
    local token="$1"

    curl \
        --noproxy '*' \
        --fail \
        --silent \
        --show-error \
        --connect-timeout "$CONNECT_TIMEOUT" \
        --max-time "$MAX_TIME" \
        -H "Authorization: Bearer ${token}" \
        -H "Accept: text/plain" \
        "$RESOLVE_URL" |
        tr -d '\r\n'
}

# 只替换 [common] 下的 server_addr，其他内容保持不变。
update_server_addr() {
    local new_addr="$1"
    local output_file="$2"

    awk -v new_addr="$new_addr" '
        BEGIN { section = ""; replaced = 0 }
        {
            line = $0
            check = line
            sub(/\r$/, "", check)

            if (check ~ /^[[:space:]]*\[[^]]+\][[:space:]]*$/) {
                section = check
                sub(/^[[:space:]]*\[/, "", section)
                sub(/\][[:space:]]*$/, "", section)
                gsub(/^[[:space:]]+/, "", section)
                gsub(/[[:space:]]+$/, "", section)
                print line
                next
            }

            if (section == "common" && check !~ /^[[:space:]]*[#;]/) {
                eq = index(check, "=")
                if (eq > 0) {
                    lhs = substr(check, 1, eq - 1)
                    gsub(/^[[:space:]]+/, "", lhs)
                    gsub(/[[:space:]]+$/, "", lhs)

                    if (lhs == "server_addr") {
                        prefix = substr(line, 1, index(line, "="))
                        print prefix " " new_addr
                        replaced = 1
                        next
                    }
                }
            }

            print line
        }
        END { if (!replaced) exit 42 }
    ' "$CLIENT_INI" > "$output_file"
}

restore_config() {
    local backup_file="$1"
    cat "$backup_file" > "$CLIENT_INI"
}

main() {
    local now
    local current_addr
    local server_port
    local connection_count
    local no_connection_since
    local elapsed
    local last_resolve_at
    local token
    local new_addr
    local normalized_new_addr
    local normalized_current_addr
    local backup_file
    local temp_file
    local config_changed=0

    case "$RESOLVE_FAMILY" in
        ipv4|ipv6|auto) ;;
        *)
            log "错误：RESOLVE_FAMILY 必须是 ipv4、ipv6 或 auto"
            exit 1
            ;;
    esac

    if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
        log "错误：需要 root 权限更新配置并重启服务"
        exit 1
    fi

    for command_name in awk curl grep systemctl; do
        if ! command -v "$command_name" >/dev/null 2>&1; then
            log "错误：找不到命令：${command_name}"
            exit 1
        fi
    done

    if [[ ! -r "$CLIENT_INI" || ! -w "$CLIENT_INI" ]]; then
        log "错误：client.ini 不可读写：${CLIENT_INI}"
        exit 1
    fi

    if [[ ! -r "$READ_TOKEN_FILE" ]]; then
        log "错误：无法读取 Token 文件：${READ_TOKEN_FILE}"
        exit 1
    fi

    mkdir -p "$STATE_DIR" || {
        log "错误：无法创建状态目录：${STATE_DIR}"
        exit 1
    }

    acquire_lock || exit 0

    current_addr="$(ini_get_common_value server_addr "$CLIENT_INI")"
    server_port="$(ini_get_common_value server_port "$CLIENT_INI")"

    if [[ -z "$current_addr" ]]; then
        log "错误：未在 [common] 中找到 server_addr"
        exit 1
    fi

    if [[ -z "$server_port" || ! "$server_port" =~ ^[0-9]+$ || "$server_port" -lt 1 || "$server_port" -gt 65535 ]]; then
        log "错误：未找到有效的 [common].server_port：${server_port:-empty}"
        exit 1
    fi

    connection_count="$(count_established_connections "$current_addr" "$server_port")" || exit 1

    if [[ ! "$connection_count" =~ ^[0-9]+$ ]]; then
        log "错误：连接统计结果无效：${connection_count}"
        exit 1
    fi

    if (( connection_count > 0 )); then
        rm -f "$NO_CONNECTION_SINCE_FILE"
        log "连接正常：${current_addr}:${server_port}，ESTABLISHED=${connection_count}"
        exit 0
    fi

    now="$(date +%s)"
    no_connection_since="$(read_file_value "$NO_CONNECTION_SINCE_FILE")"

    if [[ ! "$no_connection_since" =~ ^[0-9]+$ ]]; then
        write_file_value "$NO_CONNECTION_SINCE_FILE" "$now" || exit 1
        log "目标连接数为 0，开始计时：${current_addr}:${server_port}"
        exit 0
    fi

    elapsed=$((now - no_connection_since))
    if (( elapsed < NO_CONNECTION_GRACE_SECONDS )); then
        log "目标连接数仍为 0，已持续 ${elapsed}s，尚未达到 ${NO_CONNECTION_GRACE_SECONDS}s"
        exit 0
    fi

    last_resolve_at="$(read_file_value "$LAST_RESOLVE_AT_FILE")"
    if [[ "$last_resolve_at" =~ ^[0-9]+$ ]] && (( now - last_resolve_at < MIN_RESOLVE_INTERVAL_SECONDS )); then
        exit 0
    fi

    write_file_value "$LAST_RESOLVE_AT_FILE" "$now" || exit 1

    token="$(tr -d '\r\n' < "$READ_TOKEN_FILE")"
    if [[ -z "$token" ]]; then
        log "错误：READ_TOKEN 为空"
        exit 1
    fi

    new_addr="$(resolve_address "$token")" || {
        log "错误：查询 DDNS 地址失败"
        exit 1
    }

    if ! validate_ip_literal "$new_addr"; then
        log "错误：Worker 返回了无效地址：${new_addr:-empty}"
        exit 1
    fi

    normalized_new_addr="$(normalize_endpoint_host "$new_addr")"
    normalized_current_addr="$(normalize_endpoint_host "$current_addr")"

    backup_file="${CLIENT_INI}.ddns.bak"

    if [[ "$normalized_new_addr" != "$normalized_current_addr" ]]; then
        temp_file="$(mktemp "${CLIENT_INI}.ddns.tmp.XXXXXX")" || exit 1

        if ! cp -p "$CLIENT_INI" "$backup_file"; then
            rm -f "$temp_file"
            log "错误：备份 client.ini 失败"
            exit 1
        fi

        if ! update_server_addr "$new_addr" "$temp_file"; then
            rm -f "$temp_file"
            log "错误：更新临时配置失败；未找到 [common].server_addr"
            exit 1
        fi

        if ! cat "$temp_file" > "$CLIENT_INI"; then
            rm -f "$temp_file"
            restore_config "$backup_file" 2>/dev/null || true
            log "错误：写入 client.ini 失败，已尝试恢复备份"
            exit 1
        fi

        rm -f "$temp_file"
        config_changed=1

        if [[ "$ENABLE_CONFIGTEST" == "1" && -x "$CTUNNEL_BIN" ]]; then
            if ! "$CTUNNEL_BIN" configtest -c "$CLIENT_INI"; then
                restore_config "$backup_file" 2>/dev/null || true
                log "错误：ctunnel configtest 失败，已恢复原配置"
                exit 1
            fi
        elif [[ "$ENABLE_CONFIGTEST" == "1" ]]; then
            log "提示：未找到可执行的 ${CTUNNEL_BIN}，跳过 configtest"
        fi
    else
        log "DDNS 地址未变化：${new_addr}；由于连接持续为 0，仍将重启服务"
    fi

    if ! systemctl restart "$SERVICE_NAME"; then
        if (( config_changed == 1 )); then
            restore_config "$backup_file" 2>/dev/null || true
            systemctl restart "$SERVICE_NAME" >/dev/null 2>&1 || true
        fi

        log "错误：systemctl restart ${SERVICE_NAME} 失败"
        exit 1
    fi

    rm -f "$NO_CONNECTION_SINCE_FILE"

    if (( config_changed == 1 )); then
        log "已更新 server_addr：${current_addr} -> ${new_addr}"
    fi

    log "已重启服务：${SERVICE_NAME}"
}

main "$@"
