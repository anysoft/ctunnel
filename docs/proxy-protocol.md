# TCP PROXY Protocol

ctunnel 支持按 TCP 服务开启 PROXY Protocol v1/v2，让 VPS 本地后端看到连接光猫/随身 Wi-Fi 公网端口的真实来源 IP 和端口。

```ini
[vless]
type = tcp
remote_addr = *
remote_port = 8443
local_addr = 127.0.0.1
local_port = 443
data_encryption = true
proxy_protocol = v2
```

默认值是：

```ini
proxy_protocol = off
proxy_protocol_tlv = off
```

`proxy_protocol_tlv` 当前只接受 `off`，第一版不发送自定义 TLV。

## 工作方式

server 接受公网 TCP 连接后，从内核的 `getpeername()` 和 `getsockname()` 读取真实来源地址和本地目标地址。metadata 会绑定进已认证的 `START_STREAM`，client 不能从配置文件或公网报文伪造来源地址。

client 连接本地后端成功后，会先完整写入 PROXY header，然后才开始转发原始应用数据：

```text
local connect ok → PROXY header pending/writing → forwarding
```

如果 header 生成或写入失败，stream 会关闭，业务数据不会越过未完成的 header。

## v1 与 v2

- v1 是 ASCII 文本：`PROXY TCP4/TCP6 source destination source_port destination_port\r\n`
- v2 是二进制头，长度固定、解析更快，推荐给 Xray/VLESS、Nginx stream、HAProxy 等现代后端。

IPv4-mapped IPv6 会规范化：如果来源和目标都可转换成 IPv4，就生成 TCP4/AF_INET；否则生成 TCP6/AF_INET6。

## 后端配置提醒

支持 PROXY Protocol 的后端包括 HAProxy、Nginx stream、部分 Xray/VLESS transport、部分数据库代理和自定义 TCP 服务。不是所有 TCP 服务都支持它。

OpenSSH 默认不支持 PROXY Protocol，所以 SSH 服务应保持：

```ini
proxy_protocol = off
```

Xray/VLESS 后端通常需要启用 `acceptProxyProtocol` 或对应选项；字段名请以你使用的 Xray 版本为准。Nginx stream/HAProxy 也必须在对应 listener 上开启 PROXY Protocol 接收。

## 安全边界

PROXY header 是后端信任的元数据。如果 VPS 上其他进程或公网用户能直接连接你的后端端口，就可以自行伪造 PROXY header。因此开启后端应只监听：

- `127.0.0.1`
- Unix socket
- 或仅 ctunnel 可访问的内网/防火墙接口

推荐：

```ini
local_addr = 127.0.0.1
```

真实 IP 只对 VPS 本地后端可见。最终网站或远端服务看到的出口 IP 仍然是 VPS 的出口地址；PROXY Protocol 不会修改实际 TCP socket 源地址，也不是 TPROXY。

## 构建裁剪

相关 Kconfig：

```text
CONFIG_FEATURE_PROXY_PROTOCOL
CONFIG_FEATURE_PROXY_PROTOCOL_V1
CONFIG_FEATURE_PROXY_PROTOCOL_V2
```

Mini 默认关闭该功能。若只需要嵌入式小包加 v2，可以开启 `CONFIG_FEATURE_PROXY_PROTOCOL=y`、关闭 v1、开启 v2。
