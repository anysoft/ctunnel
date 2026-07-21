# ctunnel shells

这个目录放的是可以配合 ctunnel 使用的辅助脚本。当前这套脚本用 Cloudflare Worker + KV 做一个轻量的 Private Address Registry / Private DDNS，用来解决“ctunnel server 所在光猫、随身 Wi-Fi、家庭网关公网地址变化后，ctunnel client 如何自动找到新地址”的问题。

## 文件说明

```text
shells/
├── worker.js                 Cloudflare Worker，提供地址上报和解析 API
├── private-ddns-report.sh    设备端上报脚本，适合 Buildroot / BusyBox ash
└── ctunnel-ddns-resolve.sh   VPS/client 侧解析脚本，适合 Linux + Bash + systemd
```

推荐部署形态：

```text
光猫/随身 Wi-Fi
  ctunnel server
  private-ddns-report.sh
        │ PUT /v1/ddns/report/home/router
        ▼
Cloudflare Worker + KV
        ▲
        │ GET /v1/ddns/resolve/home/router
  ctunnel-ddns-resolve.sh
  ctunnel client
VPS / macOS / Linux 客户端机器
```

## 它解决什么问题

ctunnel 是反向隧道：server 通常跑在光猫、随身 Wi-Fi、家庭网关这类边缘设备上，client 跑在 VPS 或开发机上。现实里边缘设备的公网 IPv4 / IPv6 可能变化，client 配置里的 `server_addr` 就会过期。

这套 Private DDNS 的流程是：

1. `private-ddns-report.sh` 在边缘设备上检测 PPP 接口地址变化。
2. 地址变化后，它分别查询公网 IPv4 / IPv6。
3. 它用 `WRITE_TOKEN` 把完整地址快照写入 Cloudflare KV。
4. `ctunnel-ddns-resolve.sh` 在 client 侧监控到 ctunnel 到 server 的连接持续为 0。
5. 它用 `READ_TOKEN` 从 Worker 读取最新地址。
6. 如果地址变化，就更新 `client.ini` 的 `[common].server_addr`。
7. 可选执行 `ctunnel configtest`。
8. 重启 `ctunnel-client.service`。

注意：这是私有 registry，不是公网 DNS。它的目标是给你自己的 ctunnel 部署使用，不提供通用域名解析能力。

## Worker API

`worker.js` 提供三个接口：

```text
GET /health
GET /v1/health
PUT /v1/ddns/report/{record}
GET /v1/ddns/resolve/{record}
```

`record` 可以有 1 到 4 层路径，例如：

```text
home/router
home/china/primary/router
```

路径段只允许：

```text
a-z A-Z 0-9 . _ -
```

Worker 会把记录存到 KV key：

```text
ddns:v1:<record parts joined by colon>
```

例如：

```text
/v1/ddns/report/home/router
```

对应：

```text
ddns:v1:home:router
```

### 上报地址

```bash
curl -X PUT "https://your-worker.example.com/v1/ddns/report/home/router" \
  -H "Authorization: Bearer $WRITE_TOKEN" \
  -H "Content-Type: application/json" \
  --data-binary '{
    "addresses": {
      "ipv4": "203.0.113.10",
      "ipv6": "2001:db8::10"
    }
  }'
```

这是完整快照 PUT。`ipv4` 和 `ipv6` 两个字段都必须出现；某个协议族不可用时写 `null`：

```json
{
  "addresses": {
    "ipv4": null,
    "ipv6": "2001:db8::10"
  }
}
```

Worker 会校验地址格式，并保存：

- `schema_version`
- `id`
- `revision`
- `addresses.ipv4.address`
- `addresses.ipv4.updated_at`
- `addresses.ipv6.address`
- `addresses.ipv6.updated_at`
- `last_report.observed_address`
- `created_at`
- `updated_at`

如果地址没有变化，Worker 不会重复写 KV。

### 解析地址

返回完整 JSON：

```bash
curl "https://your-worker.example.com/v1/ddns/resolve/home/router" \
  -H "Authorization: Bearer $READ_TOKEN"
```

返回纯文本地址，适合 shell 脚本解析：

```bash
curl "https://your-worker.example.com/v1/ddns/resolve/home/router?format=text&family=ipv6" \
  -H "Authorization: Bearer $READ_TOKEN"
```

`family` 支持：

```text
ipv4
ipv6
auto
```

`auto` 优先返回 IPv6，没有 IPv6 时返回 IPv4。

## Cloudflare 部署

创建 KV namespace：

```bash
wrangler kv namespace create ADDR_KV
```

`wrangler.toml` 示例：

```toml
name = "ctunnel-private-ddns"
main = "shells/worker.js"
compatibility_date = "2026-07-21"

[[kv_namespaces]]
binding = "ADDR_KV"
id = "你的 KV namespace id"
```

配置两个 Worker secret：

```bash
wrangler secret put READ_TOKEN
wrangler secret put WRITE_TOKEN
```

部署：

```bash
wrangler deploy
```

健康检查：

```bash
curl "https://your-worker.example.com/v1/health"
```

## Token 生成

建议使用两个不同 token：

```text
READ_TOKEN   client 侧读取地址使用
WRITE_TOKEN 设备端上报地址使用
```

macOS / Linux：

```bash
openssl rand -base64 32
```

一次生成两个：

```bash
READ_TOKEN="$(openssl rand -base64 32)"
WRITE_TOKEN="$(openssl rand -base64 32)"

printf 'READ_TOKEN=%s\n' "$READ_TOKEN"
printf 'WRITE_TOKEN=%s\n' "$WRITE_TOKEN"
```

Python 方式：

```bash
python3 -c 'import secrets; print(secrets.token_urlsafe(32))'
```

token 文件建议权限：

```bash
install -m 600 /dev/null /etc/ctunnel/secrets/ddns-read-token
install -m 600 /dev/null /usr/sbin/apps/etc/cfregister/ddns-write-token
```

然后分别写入 token。

## 设备端：private-ddns-report.sh

`private-ddns-report.sh` 适合放在光猫、随身 Wi-Fi、Buildroot 设备上运行。它使用 `/tmp` 保存状态，避免周期性写 Flash。

脚本默认配置：

```sh
PPP_IFACE="ppp0"
WORKER_BASE_URL="https://yourdomain.com"
CA_CERT_FILE="/usr/sbin/apps/etc/ssl/certs/ca-certificates.crt"
DDNS_RECORD="home/router"
WRITE_TOKEN_FILE="/usr/sbin/apps/etc/cfregister/ddns-write-token"
PUBLIC_IP_API="https://api.ip.sb/ip"
STATE_DIR="/tmp/ctunnel-private-ddns"
LOCK_DIR="/tmp/ctunnel-private-ddns-report.lock"
```

最少需要修改：

```sh
PPP_IFACE="你的拨号接口"
WORKER_BASE_URL="https://你的 Worker 域名"
DDNS_RECORD="home/router"
WRITE_TOKEN_FILE="/path/to/ddns-write-token"
CA_CERT_FILE="/path/to/ca-certificates.crt"
```

运行：

```bash
sh shells/private-ddns-report.sh
```

cron 示例：

```cron
*/5 * * * * /usr/sbin/apps/sbin/private-ddns-report.sh >/dev/null 2>&1
```

脚本只有在本地 PPP 地址变化、且公网地址变化时才会写 Worker/KV。PPP 地址变化但公网地址没变时，只更新本地状态。

## client 侧：ctunnel-ddns-resolve.sh

`ctunnel-ddns-resolve.sh` 适合跑在 VPS / Linux client 侧，用来自动修复 `client.ini` 里的 `server_addr`。

脚本默认配置：

```bash
WORKER_BASE_URL="https://yourdomain.com"
DDNS_RECORD="home/router"
READ_TOKEN_FILE="/etc/ctunnel/secrets/ddns-read-token"
CLIENT_INI="/etc/ctunnel/client.ini"
SERVICE_NAME="ctunnel-client.service"
CTUNNEL_BIN="/usr/local/bin/ctunnel"
RESOLVE_FAMILY="ipv6"
```

工作方式：

1. 读取 `CLIENT_INI` 的 `[common].server_addr` 和 `[common].server_port`。
2. 用 `ss` 或 `netstat` 检查到该地址和端口的 `ESTABLISHED` TCP 连接数。
3. 如果连接数大于 0，认为 ctunnel 正常，直接退出。
4. 如果连接数持续为 0 超过 `NO_CONNECTION_GRACE_SECONDS`，向 Worker 查询最新地址。
5. 若地址变化，备份并更新 `client.ini`。
6. 如果 `ENABLE_CONFIGTEST=1`，执行 `ctunnel configtest -c CLIENT_INI`。
7. 重启 `ctunnel-client.service`。
8. 如果地址没变但连接持续为 0，也会重启服务。

运行：

```bash
sudo bash shells/ctunnel-ddns-resolve.sh
```

systemd timer 或 cron 示例：

```cron
* * * * * /usr/local/sbin/ctunnel-ddns-resolve.sh >/dev/null 2>&1
```

如果你希望优先解析 IPv6：

```bash
RESOLVE_FAMILY=ipv6 bash shells/ctunnel-ddns-resolve.sh
```

如果你的环境只有 IPv4：

```bash
RESOLVE_FAMILY=ipv4 bash shells/ctunnel-ddns-resolve.sh
```

## 配合 ctunnel 的建议配置

client.ini：

```ini
[common]
mode = client
server_addr = 2001:db8::10
server_port = 7000
client_id = vps
identity_private_key = /etc/ctunnel/client.key
server_public_key = /etc/ctunnel/server.pub
```

`ctunnel-ddns-resolve.sh` 只会替换 `[common]` 下的 `server_addr`，不会改其它服务段。

建议：

- `server_addr` 初始值可以先写当前可用地址。
- `server_port` 保持 ctunnel server 控制端口。
- `DDNS_RECORD` 在设备端和 client 侧必须一致。
- `RESOLVE_FAMILY` 要和你的 ctunnel server 实际可达协议族一致。

## 安全建议

- `READ_TOKEN` 和 `WRITE_TOKEN` 必须不同。
- 边缘设备只保存 `WRITE_TOKEN`。
- VPS/client 侧只保存 `READ_TOKEN`。
- token 文件权限建议 `0600`。
- 不要把 token 写入 git。
- KV 中只保存地址，不要保存私钥、密码、Cookie。
- Worker 使用 `Authorization: Bearer <token>`，不要把 token 放 query string。
- 如果 token 泄露，立即更新 Worker secret 并重新部署。

## 已知边界

- 这是私有 DDNS/registry，不是权威 DNS。
- Cloudflare KV 有最终一致性延迟；地址刚上报后，极短时间内读取到旧值是可能的。
- `private-ddns-report.sh` 默认依赖 `ip`、`awk`、`curl`。
- `ctunnel-ddns-resolve.sh` 默认依赖 Bash、`curl`、`awk`、`grep`、`systemctl`，并需要 root 权限修改配置和重启服务。
- Worker 不信任 Cloudflare 看到的来源地址作为 DDNS 结果；真正保存的是设备端提交的完整 IPv4/IPv6 快照。
