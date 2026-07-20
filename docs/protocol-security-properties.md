# 协议期望安全性质

本文件列出 wire v3 的目标和当前证据，不构成证明。

| 性质 | 机制 | 当前状态 |
|---|---|---|
| server authentication | 固定 server public key 验证 server-role transcript signature | 实现与单元测试支持；未形式化证明 |
| client authentication | 精确 client_id -> 唯一 authorized record -> public key，验证 client-role signature | 实现和重复配置拒绝测试支持 |
| session key secrecy | 新鲜 X25519 + transcript-salted keyed BLAKE2b KDF | 依赖原语和实现假设；未形式化证明 |
| forward secrecy | 每控制连接新鲜 ephemeral X25519，长期签名 key 不参与 DH | 设计目标成立；端点内存/随机数失陷除外 |
| control integrity/confidentiality | directional XChaCha20-Poly1305，完整 frame header AAD | 实现与 tamper/sequence 测试支持 |
| DATA confidentiality/integrity when required | per-stream/per-direction XChaCha20-Poly1305 record | 集成捕获验证密文不出现明文标记；tamper/sequence 测试支持 |
| replay resistance | control/DATA exact sequence；work replay bitmap；fresh session | 部分测试支持；无持久化跨重启 replay database |
| cross-session separation | transcript-dependent session keys/session ID | 设计支持；未形式化证明 |
| cross-stream separation | DATA KDF 绑定 stream/random/work/service/mode/direction | v3 实现和向量测试支持 |
| role separation | handshake role context、方向密钥、work/stream 消息类型与角色域 | v3 实现支持 |
| downgrade resistance | offered mask 与 selected cipher 位于签名 transcript；header 只接受 v3 | 单 cipher v3 支持，不允许 silent fallback |
| authorization isolation | client-specific ACL + 全局已绑定端点相交检查 | wildcard/specific 与控制端口冲突测试支持 |
| bounded unauthenticated cost | 有界 incoming 表、单来源上限、绝对 deadline、异步握手 | 实现及慢首帧集成测试支持；仍需外部 DDoS 限速 |

## 需要后续证明或独立验证的命题

- transcript 编码单射性和 unknown-key-share/role/cross-protocol 抵抗；
- 自定义 BLAKE2b extract/expand 的安全归约和全部标签域分离；
- 所有连接、重连、stream 生命周期中 `(key, nonce)` 唯一；
- work/stream authenticator 对 session、identity、service、stream、work、mode、方向和类型的完整绑定；
- 事件驱动状态机在任意拆包、粘包、超时和错误顺序下不会默认成功或遗留资源；
- 所有目标平台上的整数、对齐、socket 类型、文件权限和 CSPRNG 行为一致。

任何修改 transcript、KDF、nonce、AAD、work/stream authenticator 或握手状态机的修复都必须提升 wire protocol version，并默认拒绝旧版本，不得自动降级。
