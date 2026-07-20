# ctunnel 第一阶段安全审计报告

> 状态：内部代码审查与加固记录，不是独立第三方安全审计，也不是形式化证明。
>
> 审计基线：`59c12db272020f18cd9cfd466ed31dff7505dfcd`。本文件先于修复代码创建，现已补记本轮加固状态与验证结果。

## 审计摘要

本轮审查覆盖密码协议、认证与授权、C 内存安全与状态机、构建/配置/供应链四层。修复前确认 0 个 Critical、2 个 High、6 个 Medium、4 个 Low、2 个 Informational 发现。最重要的结论不是密码原语失效，而是事件循环中的阻塞协议操作允许远程可用性攻击，以及监听地址重叠可破坏合法客户端之间的服务隔离。

没有发现仅凭网络位置即可伪造 Monocypher EdDSA-BLAKE2b 身份签名、计算会话密钥或篡改已建立控制通道而不触发 AEAD 失败的证据。该结论只描述已检查实现和测试结果，不能外推为“协议已证明安全”。

| 严重程度 | 修复前数量 |
|---|---:|
| Critical | 0 |
| High | 2 |
| Medium | 6 |
| Low | 4 |
| Informational | 2 |

## 加固结果摘要

本轮已完成可在当前仓库内安全落地的修复：wire protocol 提升至 v3 并默认拒绝 v2；未认证握手与等待 READY 改为有界异步状态；全局拒绝控制端口、跨 client、wildcard/specific 监听相交；work/stream/DATA 派生绑定完整语义上下文；ACTIVE 状态使用明确消息白名单；配置、授权和密钥文件采用严格解析与 POSIX 安全打开；文件日志加入硬上限；warning gate、独立协议向量、nonce 重复 hook、DATA/key fuzz 及加密/明文捕获集成测试已加入。

CTSEC-008 标为 Mitigated：当前文件受硬上限约束，按日轮转受保留天数约束，但 stderr 仍由部署环境负责限额，且 `log_rotate_days=0` 是管理员显式接受无限历史保留。CTSEC-013 保持 Accepted risk；CTSEC-014 由 CI 矩阵覆盖但不能用本机结果替代异平台执行。

## 审计范围

- `src/protocol`、`src/crypto`、`src/net`、`src/client`、`src/server` 中的 wire v2、握手、KDF、AEAD、work/stream 绑定和转发实现；
- 配置、授权、密钥文件、日志、事件后端、随机数和对象生命周期；
- CMake、Kconfig、CI、交叉构建配置和内置 Monocypher/Kconfiglib；
- 单元、集成、Sanitizer、静态分析和 fuzz 构建入口。

不在范围内：Monocypher 原语本身的重新密码分析；操作系统内核、libc、DNS、硬件 RNG、编译器的独立审计；真实生产网络、Windows ACL、路由器/防火墙；上层 TCP 服务；侧信道和故障注入实验；完整 DDoS 压测；正式模型检验。

## 审计基线

| 项目 | 记录 |
|---|---|
| Commit | `59c12db272020f18cd9cfd466ed31dff7505dfcd` |
| Commit 时间 | 2026-07-20T00:01:21+08:00 |
| Monocypher | 4.0.3，vendored；`monocypher.c` SHA-256 `57eb914f...f64be`，头文件 `c494da71...facd` |
| 编译器 | Apple Clang 21.0.0，arm64-apple-darwin25.5.0 |
| CMake | 4.4.0 |
| Python | 3.11.15（CMake 发现的构建解释器为 3.14.6） |
| 宿主 | macOS 26.5.2 arm64，Darwin 25.5.0 |
| 当前 `.config` | both、kqueue、IPv4+IPv6、DATA encryption、work pool；hash `4e2b7a3e83147bf9` |
| 本地目标 | Default/Full both；Mini IPv6-only server-only/client-only；DATA enabled/disabled |
| CI 声明目标 | Linux GCC/Clang、macOS x86_64/arm64、Windows；ARMv5/v7/aarch64、MIPS/MIPSel、i686 |

内置 Monocypher 的版本、上游归档哈希、逐文件哈希和许可证均有记录，构建默认使用明确的 vendored 路径。未发现 CI 在线下载未固定密码库或优先链接系统同名 Monocypher 的路径。

## 威胁模型与信任边界

攻击者可观察、注入、删除、重放、重排序网络字节，主动连接公网控制端口，并可持有一个合法但受限的 client 身份。还考虑能修改部署目录中文件的本地低权限攻击者。服务端/客户端主机、进程地址空间、配置和固定公钥文件属于信任边界内；网络、DNS、外部用户连接和上层服务属于边界外。详细模型见 `docs/threat-model.md`。

```text
外部用户 --TCP--> [server public listener]
                       |
不可信网络 --TCP--> [server control/work accept]
                       |  身份认证 + 控制 AEAD + work/stream MAC
                       v
                  [client process] --TCP--> [local service]

配置/授权/私钥文件 --> 进程信任根
构建脚本/vendored Monocypher --> 发布二进制信任根
```

攻击面包括控制端口、公开服务监听器、控制/工作/DATA 解析器、连接池、配置和授权文件、密钥工具、日志路径、命令行/环境覆盖、事件后端、交叉构建和供应链更新流程。

## 从源码恢复的协议与状态机

### 控制握手

```text
client TCP_CONNECTED
  -> CLIENT_HELLO_SENT
  -> SERVER_HELLO_RECEIVED + SERVER_AUTHENTICATED
  -> CLIENT_AUTH_SENT
  -> SESSION_KEYS_DERIVED
  -> encrypted AUTH_OK_RECEIVED
  -> SESSION_ESTABLISHED -> SERVICES_REGISTERING -> ACTIVE

server TCP_CONNECTED
  -> CLIENT_HELLO_RECEIVED
  -> authorized client_id/public key selected
  -> SERVER_HELLO_SENT
  -> CLIENT_AUTH_RECEIVED + CLIENT_AUTHENTICATED
  -> SESSION_KEYS_DERIVED
  -> encrypted AUTH_OK_SENT
  -> SESSION_ESTABLISHED -> ACTIVE
```

修复前实现用同步函数调用的控制流表示握手状态，没有独立枚举。握手阶段只调用 plain frame API；`AUTH_OK` 是第一个加密帧。认证完成后才把 server peer 标为 active，服务注册和 work session 查找因此不能在认证前成功。但 ACTIVE 阶段对部分不期望的已认证消息会静默忽略，见 CTSEC-005。

| 逻辑状态 | 允许接收 | 失败行为 |
|---|---|---|
| server 等待 CLIENT_HELLO | `CLIENT_HELLO`，plain | 关闭 socket |
| client 等待 SERVER_HELLO | `SERVER_HELLO`，plain | 关闭 socket，擦除临时秘密 |
| server 等待 CLIENT_AUTH | `CLIENT_AUTH`，plain | 关闭 socket，擦除临时秘密 |
| client 等待 AUTH_OK | `AUTH_OK`，encrypted seq=1 | 关闭 socket，擦除 session keys |
| server ACTIVE | REGISTER_SERVICE、PING、PONG、GOAWAY | 解析/认证失败关闭；其他类型当前被忽略 |
| client ACTIVE | REQUEST_WORK_CONNECTION、PING、GOAWAY | 解析/认证失败关闭；其他类型当前被忽略 |

加固后的实际转换和允许集合如下；状态由对象所在的有界容器和阶段字段显式表示，不使用零值暗示“已认证”。

| 端点/状态 | 允许发送 | 允许接收 | 超时、重复或非法转换 |
|---|---|---|---|
| client TCP_CONNECTED | CLIENT_HELLO | SERVER_HELLO | 绝对 handshake timeout；其他类型/非规范 header 关闭并擦除临时秘密 |
| client SERVER_AUTHENTICATED | CLIENT_AUTH | encrypted AUTH_OK seq=1 | 签名、cipher、identity fingerprint、session/header 任一不符关闭；AUTH_OK 不会提前接受 |
| client SERVICES_REGISTERING | REGISTER_SERVICE、PING/PONG | REGISTER_OK、REQUEST_WORK、PING/PONG | reply 必须匹配当前 service ID；握手消息、提前/重复回复关闭 |
| client ACTIVE | PING/PONG、WORK_BIND、新流 READY/FAILED、GOAWAY | REQUEST_WORK、START_STREAM、PING/PONG、GOAWAY | 明确白名单；未知、重复握手和角色错误消息关闭对应会话/流 |
| server TCP_CONNECTED | 无 | CLIENT_HELLO 或 WORK_BIND 首帧 | incoming 数组和单来源数量有界；绝对 deadline；WORK 必须找到已 active session |
| server HELLO_RECEIVED | SERVER_HELLO | CLIENT_AUTH | 分段事件驱动；重复 HELLO、其他类型或 deadline 到期关闭并擦除 handshake 对象 |
| server CLIENT_AUTHENTICATED | encrypted AUTH_OK | 无 | 只有双向签名、X25519、KDF 和 AUTH_OK 发送均成功后才插入 active peer |
| server ACTIVE | REGISTER_OK/FAILED、REQUEST_WORK、START_STREAM、PING/PONG、GOAWAY | REGISTER_SERVICE、PING/PONG、GOAWAY | 明确白名单；认证前无法注册、bind listener 或关联 work；非法类型关闭 peer |
| server START_SENT/WAIT_READY | START_STREAM | STREAM_READY/FAILED | 独立有界 starting 对象；session/work/service/mode/stream/role/MAC 不符或 deadline 到期关闭两端 socket |
| 任一 GOAWAY/CLOSED | GOAWAY（尽力而为） | 无 | 关闭 listener、idle work、pending、starting、relay 和 control，并擦除 session keys |

TCP 拆包时只有完整、上限内的 frame 才进入同步解码；不完整 frame 被短 deadline 退避并继续服务其他 fd。事件引用使用 fd 重新查找当前数组元素，避免 swap-remove 后的悬空索引或 fd 复用误绑定。

### work 与 stream

```text
client: connect -> WORK_CONNECTION_BIND(session, work_id, MAC) -> idle pool
server: verify session/MAC/replay window -> idle pool
external accept -> pending -> START_STREAM(service, mode, random, stream, MAC)
client local connect -> STREAM_READY/FAILED(stream, random, MAC)
READY -> raw relay 或 per-stream DATA AEAD relay -> close
```

控制断开时 server 会关闭该 peer 的 listener、idle work、pending 和 relay；客户端也关闭对应资源。当前绑定认证值没有完整覆盖 work ID、服务、模式、角色和消息类型，见 CTSEC-004。

## 密码学检查结果

| 项目 | 结果 |
|---|---|
| 身份签名 | 使用 Monocypher 核心 `crypto_eddsa_*`（EdDSA-BLAKE2b），不是 Ed25519/SHA-512；client 固定 server 公钥，server 由精确 client_id 选择公钥 |
| X25519 | 每次握手新建随机临时私钥；拒绝全零共享秘密；握手失败擦除 xsk/shared |
| transcript | 规范字节编码覆盖 client_id 长度和值、双方 random、双方 ephemeral key、offered mask、selected cipher，并有 v2/角色域；未签名 C struct |
| KDF | keyed BLAKE2b-512 extract/expand，salt 为 transcript hash，64 位 LE counter；用途和方向标签分离；属于自定义组合，未形式化验证 |
| nonce | XChaCha20 24 字节 nonce=`base[16] || sequence_be64`；control/DATA 分方向、分流派生；回绕前失败 |
| AEAD | 36 字节控制头和 12 字节 DATA 记录头作为 AAD；tag 16 字节；认证成功后才推进 rx sequence |
| sequence | 控制与每流严格 `previous+1`；work ID 使用独立 64 项 replay window |
| work binding | session-specific work-auth key + session ID + 32 字节 work ID；协议字段覆盖仍不完整 |
| stream binding | session-specific stream-auth key + 部分 payload；未完整绑定 work ID/角色/所有语义 |
| DATA encryption | required 时双向走 per-stream XChaCha20-Poly1305；disabled 时业务数据明文，建立过程仍认证 |

未发现相同 session key 下主动复用 control nonce 的路径。发送序号在加密前递增，任何发送失败均终止相应会话/流而不重试重新加密；重连生成新 ephemeral key/random。测试前尚无 debug-only `(key fingerprint, nonce)` 全局重复探针，列为 CTSEC-012。

## 发现清单

| ID | 严重程度 | 组件 | 问题 | 攻击场景 | 影响 | 修复状态 | 协议版本变化 |
|---|---|---|---|---|---|---|---|
| CTSEC-001 | High | server/event loop | 未认证 accept、首帧探测和完整握手在唯一事件线程阻塞 | 远端连接后慢发/不发首帧，连续占用 5 秒或握手超时 | 所有 client、listener、heartbeat 停止服务 | Fixed | 否 |
| CTSEC-002 | High | server/work stream | `START_STREAM` 后同步等待 READY 最长 5 秒 | 合法恶意 client 建 work socket但不回复 READY | 单一受限身份阻塞整个 server | Fixed | 否 |
| CTSEC-003 | Medium | authorization/listener | 没有全局检测 wildcard 与 specific listener 重叠；`SO_REUSEADDR` 在实测平台允许两者并存 | client A 绑定 `0.0.0.0:p`，client B 绑定 `127.0.0.1:p` | B 可截获/覆盖发往特定地址的 A 流量 | Fixed | 否 |
| CTSEC-004 | Medium | work/stream protocol | authenticator 未完整绑定 work ID、服务、模式、角色、方向、消息类型；READY 也未检查 header session/type | 合法端点、实现错误或未来复用把消息移植到错误 socket/语义 | 跨 work/stream 混淆防御不满足设计目标 | Fixed in v3 | 是 |
| CTSEC-005 | Medium | control state machine | ACTIVE 对不允许的握手/注册回复等消息静默忽略 | 已认证恶意 peer 注入乱序/重复控制消息 | 状态分歧、未来扩展形成默认成功路径 | Fixed | 否 |
| CTSEC-006 | Medium | config/auth parser | common 重复键、授权重复 client section/单值键未拒绝 | 本地文件拼接、部署工具重复配置 | 安全关键值解释歧义，审阅结果与运行值不一致 | Fixed | 否 |
| CTSEC-007 | Medium | key/config filesystem | 私钥权限过宽仅警告；授权文件无权限检查；读取跟随符号链接且不要求常规文件 | 能写部署目录或替换链接的本地用户 | 身份密钥/授权策略被替换或暴露 | Fixed on POSIX; Windows ACL documented | 否 |
| CTSEC-008 | Medium | logging/DoS | 日志仅按日期轮转，无单文件/总字节上限；远程失败仍可产生日志 | 分布式来源持续触发认证/注册失败 | 16MB Flash 或系统磁盘耗尽 | Mitigated | 否 |
| CTSEC-009 | Low | frame canonicality | plain handshake/work 帧未要求无关 session/stream/sequence 字段为零 | 网络修改无语义字段或跨协议重用编码 | 编码不唯一，增加跨协议/未来扩展风险 | Fixed in v3 | 是 |
| CTSEC-010 | Low | key parser | 文件读取未以 `lstat/open/fstat` 固化对象，末尾空白规则比格式规范宽 | 本地并发替换或带控制空白的 key 文件 | 审计性和文件身份保证不足 | Fixed on POSIX | 否 |
| CTSEC-011 | Low | build/static analysis | 必需完整严格警告集未在 CI 强制；本地 Full 严格构建失败 | 新缺陷被较弱 warning gate 漏过 | 防御深度下降 | Fixed | 否 |
| CTSEC-012 | Low | tests/fuzz | 无 nonce 重复 hook；fuzz 仅浅层 frame/hello/config，缺 DATA/work/stream/key parser | 同实现双端测试隐藏共同错误 | 回归检测覆盖不足 | Fixed/expanded | 否 |
| CTSEC-013 | Informational | custom KDF/protocol | 自定义 BLAKE2b KDF 与协议未形式化或独立验证 | 设计层未知错误 | 无法证明所列安全性质 | Accepted risk | 否 |
| CTSEC-014 | Informational | platform validation | 本机不能执行 Linux/Windows/ARM/MIPS 动态测试或 MSan/CodeQL | 平台条件分支存在未观测缺陷 | 验证结论受平台限制 | CI coverage; residual | 否 |

## 发现详情与修复要求

### CTSEC-001：未认证连接阻塞全局事件循环（High）

证据：`server.c` 接受 socket 后直接调用轮询 `MSG_PEEK` 的 `first_type()`，内部最多循环 500 次并 sleep 10ms；随后直接调用包含多次 `recvall/sendall` 的 `ct_handshake_server()`。这些调用发生在唯一事件循环的 ready-event 处理体内。攻击者无需身份，仅需 TCP 可达。

修复应把未认证 socket 作为有界状态对象加入事件循环，限制全局/单 IP 数量，使用单一绝对握手 deadline，逐步解析固定上限 frame，并在基本格式检查后才做签名。短期若不能完成异步状态机，至少不能把多秒等待放在 event callback 中。

### CTSEC-002：合法 client 可阻塞全局 stream 启动（High）

证据：外部 accept 后 `match_pending()` -> `start_relay()` 同步发送 START，再调用 `ct_stream_ready_recv(..., 5000)`。一个已认证 client 可以提交 work socket，收到 START 后保持静默，使所有 peer 的 control、heartbeat 和 listener 事件停止处理。

修复应引入 `START_SENT/WAIT_READY` pending-work 状态，由事件循环异步接收 READY 并按 deadline 回收。

### CTSEC-003：跨 client 监听重叠（Medium）

证据：服务表仅属于单个 `server_peer`；注册前只检查同一 peer 的 service ID。macOS 实测在两个 socket 都设置 `SO_REUSEADDR` 时，`0.0.0.0:p` 与 `127.0.0.1:p`、`::p` 与 IPv4 同端口可同时 listen。specific listener 会改变同端口流量归属。所需前提是两个合法身份的 ACL 地址/端口重叠。

修复应基于已绑定 socket 的规范 family/address/port，在全 server 范围拒绝 exact/exact 和 wildcard/specific 相交，并把控制 listener 纳入冲突检查。

### CTSEC-004：stream 绑定不完整（Medium）

`WORK_CONNECTION_BIND` 的 MAC 覆盖标签、session 和 work ID，但验证后不保存 work ID。START MAC 覆盖 service、mode、random、stream；READY MAC 只覆盖 ready 标签、stream、random、ok。协议要求的 client identity context、work ID、双方方向、消息类型以及 READY 的 service/mode 均未完整出现；READY 接收还未显式校验 header session 和只允许 READY/FAILED。

修复需要改变 wire authenticator，必须提升 protocol version，默认拒绝 v2，不允许兼容回退。

### CTSEC-005 至 CTSEC-012

- 为 control ACTIVE 明确允许集合，任何其他已认证消息导致连接关闭；重复注册、握手重注入和提前回复均不得被忽略。
- parser 跟踪 section 内已见单值键；授权 client ID 唯一；只有 `allow_remote_port` 可重复。
- POSIX 使用不跟随链接的 open/fstat，拒绝非普通文件；私钥必须是 owner-only，授权文件至少拒绝 group/world writable。Windows 明确记录 ACL 需由部署者控制。
- 日志加入单文件硬上限和有界轮转总量，认证失败日志采样；Mini 支持 stderr-only/无文件日志。
- plain 消息对不使用的 header 字段要求零并将语义字段纳入认证值。
- CI 对项目源码启用完整 warning 集，不把项目 warning 施加给 Monocypher；扩充独立向量和 fuzz harness。

## 核心问题逐项回答（修复前）

1. 网络攻击者冒充 server：未发现可行路径；需要伪造固定 server key 的 EdDSA-BLAKE2b 签名。
2. 网络攻击者冒充 client：未发现可行路径；client_id、transcript 与授权公钥绑定。
3. MITM：主动改写 ephemeral/random/cipher 会破坏 transcript 签名；纯转发仍可观察/阻断流量。
4. 握手重放：双方随机数和 ephemeral key 使旧签名不适配新会话；未发现接受旧 SERVER_HELLO 的路径。
5. work 重放/抢占：session-specific MAC 和 64 项 window 拒绝重复；绑定覆盖不完整见 CTSEC-004。
6. stream 错绑/跨会话：session-specific key 和 random 提供保护，但未绑定 work ID/全部语义，不能声称完全满足。
7. key/nonce 重用：实现设计上分会话/方向/流；未有重复 hook 证明所有路径，见 CTSEC-012。
8. 降级：唯一 cipher 选择被签名；DATA mode 在注册/START MAC 中出现，客户端尚未对本地配置做一致性复核。
9. UKS：client_id 在 transcript，验证 key 由其授权记录选取，server key 固定；未发现已知路径。
10. 角色混淆：握手签名上下文分角色；work/stream 角色域仍不完整。
11. 跨协议：v2 标签提供域分离；plain header 非规范字段和不完整 authenticator 留有加固项。
12. 身份与 ephemeral：双方签名同一含双方 ephemeral 的 transcript；身份 fingerprint 是隐式由验证 key 绑定，不作为显式字段。
13. 控制篡改：完整 header 是 AAD，payload/tag 修改会失败。
14. DATA=true：双向机密性/完整性/顺序重放保护由每流 AEAD 提供；仍暴露流量元数据并可被截断/DoS。
15. DATA=false：业务字节明文；stream 建立有 session MAC，但完整绑定缺口见 CTSEC-004。
16. 合法 client 监听未授权端口：端口与地址 ACL 在 bind 前检查；未发现直接绕过，但地址采用字符串授权而非解析后规范地址。
17. 覆盖其他 client：可以在重叠 ACL 下通过 wildcard/specific socket 组合影响归属，CTSEC-003。
18. 未认证资源耗尽：可以阻塞事件循环，CTSEC-001；并缺并发未认证连接状态上限。
19. 恶意配置：长度/整数总体有界，但重复键、文件权限/链接和授权歧义存在。
20. 恶意网络帧：当前 ASan/UBSan 与有限 fuzz 未触发崩溃；这不是不存在 UAF/越界的证明。

## `data_encryption=true` 后攻击者仍能做什么

网络攻击者仍能观察 server/client 端点 IP、连接数量、流量大小、方向和时序，主动断开、延迟或丢弃 TCP，扫描公开服务端口并实施 DoS。它不隐藏隧道存在，也没有流量填充或抗 DDoS 能力。

私钥被盗后，攻击者可冒充对应身份；主机被攻陷后可在加密前/解密后读取或修改数据。配置或授权文件被替换会改变信任根和监听权限。实现中的状态机、nonce、重放或内存安全错误仍可能破坏协议目标。日志、计时、资源使用和上层服务自身漏洞不由 DATA AEAD 解决。

必须区分：Monocypher 原语的安全性不等于 ctunnel 协议设计安全；协议设计不等于 C 实现无缺陷；实现正确不等于密钥/配置/主机部署安全；隧道安全也不等于上层应用安全。

## 工具与验证结果（修复前）

| 检查 | 结果 |
|---|---|
| Default Debug + ASan + UBSan | 构建通过 |
| Unit | 通过 |
| Integration | encrypted/raw、pool、reconnect/reregister 通过 |
| cppcheck | `warning,performance,portability` 未产生诊断；需人工确认，不能解释为无漏洞 |
| 严格 warning 集 | Full 失败：test hook 原型可见性、`vfprintf` nonliteral gate |
| libFuzzer | harness 编译，宿主缺 `libclang_rt.fuzzer_osx.a`，未运行 |
| Mini server-only | macOS arm64、IPv6-only、DATA off 构建通过 |
| Mini client-only | macOS arm64、IPv6-only、DATA off 构建通过 |
| Full both | macOS arm64、IPv4/IPv6、DATA on 构建通过 |
| Linux/Windows/ARM/MIPS | 仅审阅 CI/toolchain；本轮本机未执行 |
| MSan/TSan/CodeQL/clang analyzer | 本轮未执行；TSan 对当前单线程主循环价值有限 |

## 加固后验证

| 检查 | 结果 |
|---|---|
| Full Debug + ASan + UBSan + test hooks + warnings-as-errors | 构建通过，CTest 4/4 通过 |
| Default ASan + UBSan | CTest 4/4 通过 |
| 协议独立向量 | Python 标准库编码器与记录的 v3 header/KDF/work/stream 向量一致；C 单元向量一致 |
| 单元恶意输入 | control/DATA 重放、错误 READY session、错误 work/service 上下文、重复配置、宽松私钥权限、日志上限均拒绝 |
| 集成 | 慢首帧不阻塞合法 client；6 路并发 pool、重连/重注册、大响应通过；隧道捕获中 encrypted 标记不可见、raw 标记可见 |
| 严格编译 | 完整要求的 warning 集加 `-Werror` 通过；CI 由 `CTUNNEL_WARNINGS_AS_ERRORS=ON` 强制 |
| cppcheck | `warning,performance,portability` 通过，无诊断 |
| 人工 ownership/lifecycle | 检查 accept/incoming/control/work/pending/starting/relay 的唯一 owner、swap-remove、超时与 peer teardown；延迟事件按 fd 重新查找，未找到即丢弃 |
| Mini server/client | macOS arm64、IPv6-only、DATA off 分别构建通过 |
| libFuzzer | 6 个 harness 覆盖 frame、handshake、DATA header、service registration、config 和 key file；本机链接因 Apple Command Line Tools 缺 `libclang_rt.fuzzer_osx.a` 未运行；Linux CI 配置每目标 20,000 次 smoke run |
| 异平台 | Linux/macOS 双架构/Windows/交叉架构由 CI 矩阵声明覆盖；本报告不把未在当前主机执行的 job 记为已通过 |

构建安全状态：Default 与 Full both、Mini server-only/client-only、DATA on/off、IPv6-only 和 macOS arm64 已在本机实际构建；Linux GCC/Clang、macOS x86_64、Windows x86_64/i686、ARMv5/v7/aarch64、MIPS/MIPSel 配置由 CI/toolchain 审阅覆盖，需以对应 CI job 的真实结果作为发布门禁。本轮未在当前 macOS 主机执行 ARM32、MIPS 或 Windows 二进制，因此不宣称这些平台已动态验证。

## 残余风险

即使所有 Open 项修复并回归通过，仍残留自定义协议/KDF 未被独立审查或形式化验证、流量分析、端点暴露、DDoS、主机/私钥失陷、DNS和文件系统部署错误、上层服务漏洞、平台条件代码差异、编译器/OS/Monocypher供应链风险。DATA disabled 明确不提供业务数据机密性或完整性。DATA enabled 不提供隐藏长度/时序、抗截断可用性或应用语义安全。
