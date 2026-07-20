# 线协议 v3

所有整数使用网络字节序，字段逐一编码，不传输 C 结构体布局。v3 因加强 work、stream 和 DATA 上下文绑定而与 v2 有意不兼容；实现看到非 v3 帧即拒绝，不提供自动降级。

## 帧

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 4 | magic `CTUN`（`0x4354554e`） |
| 4 | 1 | 协议版本 `3` |
| 5 | 1 | 消息类型 |
| 6 | 1 | flags（`1` 表示控制 payload 已加密） |
| 7 | 1 | header 长度 `36` |
| 8 | 4 | payload 长度 |
| 12 | 8 | session ID |
| 20 | 8 | stream ID |
| 28 | 8 | sequence |

plain 握手消息未使用的 session、stream、sequence 必须为零；work bind 要求 stream、sequence 为零；START/READY 的 stream ID 必须与认证 payload 中的值一致。长度在分配或读取前验证，未知版本、类型、flags、非规范字段和越界长度均失败关闭。

## 握手、转录与会话密钥

`CLIENT_HELLO` 为 `client_id:u16+bytes || client_random[32] || client_ephemeral_x25519[32] || offered_ciphers:u32`。`SERVER_HELLO` 为 `server_random[32] || server_ephemeral_x25519[32] || selected_cipher:u8 || signature[64]`，`CLIENT_AUTH` 为 `signature[64]`。当前仅套件 1 `xchacha20-poly1305` 可用。

规范转录为：

```text
"ctunnel-handshake-v3" || magic:u32 || protocol_version:u8 ||
CLIENT_HELLO_payload || server_random || server_ephemeral_public ||
selected_cipher || BLAKE2b-256(client_identity_public) ||
BLAKE2b-256(server_identity_public)
```

其中 magic 是 `CTUN`，protocol version 是 3。服务端和客户端分别签名 `"ctunnel server handshake v3" || BLAKE2b-256(transcript)` 与 `"ctunnel client handshake v3" || BLAKE2b-256(transcript)`。签名算法是 Monocypher EdDSA-BLAKE2b。身份公钥 fingerprint 与验证签名实际使用的固定/授权公钥一致。双方用新鲜临时 X25519 密钥导出 shared secret，拒绝全零结果；`AUTH_OK` 是首个加密控制帧。

会话 KDF 令 `salt=BLAKE2b-256(transcript)`，`PRK=BLAKE2b-512(key=salt, message=X25519_shared)`，再连接 `BLAKE2b-512(key=PRK, message=LE64(counter)||context)`（counter 从 0 开始）并截取所需长度。任何派生失败都会终止握手，不使用部分或零输出。

固定 context 列表如下，不使用前缀匹配：

```text
ctunnel-v3/control-c2s-key
ctunnel-v3/control-s2c-key
ctunnel-v3/control-c2s-nonce
ctunnel-v3/control-s2c-nonce
ctunnel-v3/data-master
ctunnel-v3/work-auth
ctunnel-v3/stream-auth
ctunnel-v3/session-id
ctunnel-v3/data-stream-c2s-key
ctunnel-v3/data-stream-s2c-key
ctunnel-v3/data-stream-c2s-nonce
ctunnel-v3/data-stream-s2c-nonce
```

work 与 stream keyed BLAKE2b-256 输入另用完整标签 `ctunnel-v3/work-bind/client-to-server` 和 `ctunnel-v3/stream-binding`。DATA 派生先用 `BLAKE2b-256` 哈希 `ctunnel-v3/data-stream || stream || random || work || service-length/service || mode || direction` 作为 salt，再使用上列方向标签。独立 Python/C 向量见 `tests/vectors/`。

控制 payload 使用 XChaCha20-Poly1305，完整 36 字节 header 是 AAD。nonce 为 `base[16] || sequence_u64_be`。发送序号从 1 开始且在加密前递增；接收端只接受精确的前值加一；回绕、重复、乱序或 tag 失败均终止会话。

## 服务、work 与 stream 绑定

`REGISTER_SERVICE` 在已认证控制通道内携带 service ID、监听地址、端口、TCP 类型和 DATA 模式。服务端先执行身份 ACL、资源上限和全局监听端点冲突检查，再绑定公开 listener。

`WORK_CONNECTION_BIND` payload 为 `work_id[32] || mac[32]`。work ID 是 `monotonic_u64 || random[24]`。MAC 输入为版本/方向标签、session ID、client ID、消息类型和完整 work ID；服务端保存该 ID，并用 64 位滑动窗口拒绝零、重复和过旧序号。

`START_STREAM` 与 `STREAM_READY/STREAM_FAILED` 都携带并认证：session、client ID、work ID、service ID、stream ID、DATA mode、stream random、消息类型、发送角色和结果位。接收端还验证 header 的 session/type/stream/sequence。认证值只能在对应会话、work socket、服务、模式、方向和消息阶段使用。

## DATA 记录

`data_encryption=false` 时，stream 建立仍被认证，但后续应用字节原样传输，不提供业务数据机密性或完整性。

`data_encryption=true` 时，每个方向独立派生 key 和 nonce base；派生上下文包含 stream ID、stream random、work ID、service ID、DATA mode 和方向。记录格式为：

```text
ciphertext_length:u32 || sequence:u64 || ciphertext || tag[16]
```

12 字节记录头是 AAD，明文记录最大 16 KiB，sequence 从 1 开始并严格递增。header、tag、顺序或长度不合法会关闭该 relay。加密不隐藏端点、记录长度、方向、时序和连接数量，也不提供抗阻断能力。

任何修改 header、握手转录、签名上下文、KDF、nonce、AAD 或 work/stream 认证覆盖范围的变更，都必须再次提升协议版本并默认拒绝旧版本。
