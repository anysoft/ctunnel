# UDP 反向转发

ctunnel 第二阶段支持轻量 UDP 反向转发。服务端在公网侧监听 UDP 端口，外部 UDP 报文会通过已认证的控制通道转发到客户端，客户端再用本地 UDP socket 发给 `local_addr:local_port`。本地服务回复后，客户端把回复封回控制通道，服务端按原始外部源地址发回。

示例客户端服务段：

```ini
[dns]
type = udp
remote_addr = *
remote_port = 5353
local_addr = 127.0.0.1
local_port = 53
data_encryption = true
udp_idle_timeout = 60
udp_reply_timeout = 5
udp_max_sessions = 1024
udp_max_datagram_size = 1472
```

配置含义：

- `type=udp`：启用 UDP 服务。Mini defconfig 默认裁剪 UDP，普通 default/full 默认启用。
- `remote_addr`/`remote_port`：服务端公网侧 UDP 监听地址。`*` 在双栈构建中同时监听 IPv4 和 IPv6。
- `local_addr`/`local_port`：客户端内网侧 UDP 目标。
- `udp_idle_timeout`：外部源地址对应的 UDP 伪会话多久无流量后回收，默认 60 秒。
- `udp_reply_timeout`：预留给回复等待策略，当前实现使用 idle 统一回收。
- `udp_max_sessions`：单服务允许的 UDP 伪会话数，不能超过编译期 `CONFIG_MAX_UDP_SESSIONS`。
- `udp_max_datagram_size`：单个 UDP payload 上限，默认 1472，适合避免常见路径 MTU 分片。

构建开关：

```text
CONFIG_FEATURE_UDP=y
CONFIG_MAX_UDP_SESSIONS=1024
CONFIG_MAX_UDP_DATAGRAM_SIZE=1472
CONFIG_UDP_REPLAY_WINDOW=64
```

本地验证：

```bash
./build-local.sh --build-dir build-udp --type Debug --tests --no-package
scripts/udp-stress-test.sh build-udp/ctunnel
```

当前 UDP v1 明确不做 KCP、QUIC、可靠 UDP、拥塞控制、STUN/TURN、UDP over UDP，也不保证报文不丢。它只保留 UDP 报文边界和外部源地址映射，适合 DNS、探测类协议、小报文 UDP 服务，以及明确能容忍丢包的场景。
