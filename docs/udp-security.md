# UDP 安全说明

UDP 反向转发复用 ctunnel 的身份认证和控制通道 AEAD。只有通过双向公钥认证、且在服务端授权文件中允许对应 `allow_bind_addr`/`allow_remote_port` 的客户端，才能注册 UDP 监听端口。

安全边界：

- 外部 UDP 源地址和端口会被编码进隧道帧，服务端用它恢复返回路径。
- 每个 UDP 伪会话使用单调递增序号，并用 `CONFIG_UDP_REPLAY_WINDOW` 做重放窗口检查。
- 超过 `udp_max_datagram_size` 的报文会丢弃并计数，避免单个报文占用过多控制通道缓冲。
- 超过 `udp_max_sessions` 的新源地址会被拒绝，避免公网 UDP 扫描拖垮客户端。
- Mini 构建默认关闭 UDP，嵌入式环境需要时再显式打开。

注意：当前 UDP v1 把 UDP DATAGRAM 作为加密控制帧传输，因此每个隧道帧都会经过控制通道认证加密；`data_encryption=false` 目前只影响 TCP DATA 记录，不会把 UDP 控制帧降级成明文。后续如果引入独立 UDP DATA record，再单独定义其密钥派生和 nonce 规则。

建议生产配置：

- 公网 UDP 服务优先设置较小的 `udp_idle_timeout`，例如 30 到 120 秒。
- DNS 类小报文服务保持 `udp_max_datagram_size=1472`。
- VLESS/代理类高并发场景提高 `udp_max_sessions`，同时确认设备 fd 和内存足够。
- 服务端授权文件只开放必要端口，不要给未知客户端使用 `allow_bind_addr=*` 和宽泛端口范围。
