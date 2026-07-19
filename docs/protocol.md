# 线协议 v2

所有线协议整数均为无符号网络字节序。各字段逐一编码，绝不传输 C 结构体布局。版本 2 与先前第一期草案有意不兼容，因为签名算法、KDF、nonce 长度、套件名称和握手转录上下文均已变更。

## 帧头

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 4 | magic `CTUN`（`0x4354554e`） |
| 4 | 1 | 协议版本（`2`） |
| 5 | 1 | 消息类型 |
| 6 | 1 | 标志（`1` = 已加密） |
| 7 | 1 | 帧头长度（`36`） |
| 8 | 4 | payload 长度 |
| 12 | 8 | 会话 ID |
| 20 | 8 | 流 ID |
| 28 | 8 | 序号 |

payload、客户端/服务 ID、地址和缓冲区上限均为编译期 Kconfig 硬限制（Default 档位使用 1 MiB、64 字节 ID 和 128 字节地址）。字符串在线协议中仍使用 16 位长度。解码器会拒绝错误的 magic、版本和帧头长度，未知消息，超过本地编译上限的值，畸形字段，错误的加密标志或会话，以及不连续的加密序号。因此，互通双方必须使用兼容的协议限制。

## 握手与密钥派生

`CLIENT_HELLO` 携带 `client_id`、random[32]、临时 X25519 公钥[32] 和密码套件掩码。`SERVER_HELLO` 携带 random[32]、临时 X25519 公钥[32]、所选密码套件和 signature[64]。`CLIENT_AUTH` 携带 signature[64]。只支持套件 ID 1（`xchacha20-poly1305`）；不支持的提议或选择会被拒绝。

握手转录是以下字节串的连接：`"ctunnel-handshake-v2" || CLIENT_HELLO_payload || server_random || server_ephemeral_public || selected_cipher`。服务端签名 `"ctunnel server handshake v2" || BLAKE2b-256(transcript)`，客户端签名对应的 `"ctunnel client handshake v2"` 值。签名使用 Monocypher EdDSA-BLAKE2b。`AUTH_OK` 是第一个加密帧。

KDF 提取阶段使用 keyed BLAKE2b-512，其中 `key=transcript_hash`、`message=X25519_shared_secret`。扩展阶段把 `little_endian_u64(counter) || context` 输入 keyed BLAKE2b-512，连接各输出块后截断到所需长度。带版本的标签分别派生不同角色/通道的密钥和 nonce 基值，以及工作连接/流认证密钥和会话 ID。

加密 payload 长度包含 16 字节 XChaCha20-Poly1305 tag。完整 36 字节帧头作为关联数据。nonce 为 `base[16] || sequence_u64_be`。发送方在使用前递增序号；接收方只接受严格等于 `previous + 1` 的序号；序号耗尽会终止会话。

## 服务注册与工作流

`REGISTER_SERVICE` 携带服务 ID、监听地址、端口、服务类型（`1=TCP`，`2=HTTP` 和 `3=HTTPS` 保留）以及 DATA 模式（`0=required`、`1=disabled`）。第一期会拒绝保留的服务类型。

`WORK_CONNECTION_BIND` 携带工作 ID[32]（`monotonic_u64_be || random[24]`）和 keyed BLAKE2b-256 authenticator[32]，会话 ID 位于帧头中。64 序号滑动窗口允许不同套接字上的有限乱序到达，但会拒绝重复值、零和过旧值。启动/就绪消息携带服务、流、模式、连接随机数以及使用独立密钥生成的认证值。

就绪后，`disabled` 模式流直接转发原始 TCP；`required` 模式流使用 `ciphertext_length:u32 || sequence:u64 || ciphertext || tag[16]`，其中 12 字节记录头作为关联数据。明文记录最大为 16 KiB，并使用每流、每方向的密钥和 nonce 基值。

未知协议版本会被拒绝。未来任何对帧格式、握手转录覆盖范围、签名构造、密钥派生或 nonce 构造的修改都必须使用新的协议版本。
