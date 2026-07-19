# 运行时配置参考

INI 键名区分大小写。未知键、重复或无效服务、超范围整数、缺少必填字段以及不支持的服务类型都会导致配置失败。只有授权文件允许重复出现 `allow_remote_port`。

运行时配置不能增加 `.config` 在编译期裁剪掉的能力。单角色构建会拒绝相反的 `mode`；仅 IPv6 构建会拒绝数字形式的 IPv4 地址；未编译转发加密的构建会拒绝 `data_encryption=required`；未编译工作连接池的构建要求 `pool_count=0`；高于编译期上限的日志级别无效。资源数值不得超过 `docs/build-configuration.md` 列出的生成期硬限制。

服务端通用键包括：`mode`、`bind_addr`、`bind_port`、`identity_private_key`、`authorized_clients_file`、`allowed_ciphers`、`preferred_cipher`、`control_encryption=required`、`auth_mode=public-key`、心跳/握手超时、客户端/服务/流/等待连接限制，以及 `log_level`。

客户端通用键包括：`mode`、`server_addr`、`server_port`、`client_id`、身份密钥和服务端公钥路径、密码套件设置、心跳/握手/连接超时、重连延迟和抖动、`pool_count`、默认 DATA 加密模式，以及日志级别。

每个客户端服务段只接受 `type=tcp`、`remote_addr`、`remote_port`、`local_addr`、`local_port` 和 `data_encryption=required|disabled`。服务端授权段 `[client.ID]` 接受公钥路径、精确的 `allow_bind_addr`（或 `*`）、一个或多个单端口/端口范围，以及该身份专属的限制。

第一期只接受 `allowed_ciphers=xchacha20-poly1305` 和 `preferred_cipher=xchacha20-poly1305`。AES-GCM 及其他名称都会导致配置错误。控制通道强制加密。DATA `disabled` 只影响应用数据记录；工作连接和流绑定仍会经过认证。
