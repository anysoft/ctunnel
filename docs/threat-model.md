# ctunnel 威胁模型

## 资产

- server/client 长期签名私钥、固定公钥和授权映射；
- 会话共享秘密、控制/DATA/work/stream 派生密钥和 nonce 状态；
- 经隧道传输的应用明文；
- client 身份、service/stream/work 绑定完整性；
- listener、fd、内存、CPU、日志存储和事件循环可用性；
- 构建配置、vendored 依赖和发布产物完整性。

## 攻击者能力

网络攻击者能连接公网控制端口和公开服务端口，观察、注入、删除、截断、延迟、重放和重排序 TCP 字节。授权攻击者持有一个合法 client 私钥，但只应拥有其 ACL 范围。部署目录攻击者可能读/写配置、授权、日志或链接目标，但不默认拥有进程地址空间。主机 root/Administrator、内核或进程内攻击者视为信任边界已失陷。

## 安全目标

- 双方长期身份和临时 X25519 会话绑定；
- 会话密钥保密与会话间分离；
- control 完整性、机密性、顺序和重放保护；
- DATA required 时的双向机密性、完整性、分流/分方向和重放保护；
- work 与 stream 不能跨身份、会话、服务或连接移植；
- client 只能创建 ACL 允许且不与其他身份冲突的 listener；
- 恶意网络输入不能造成越界、UAF、泄漏、无限循环或无界资源增长；
- 安全功能不能被 Kconfig 或 wire negotiation 静默关闭。

## 非目标

不隐藏端点、流量大小/时序、隧道存在或公开服务端口；不吸收 DDoS；不提供 PKI、吊销、匿名性、0-RTT、持久化 replay 数据库、stream 恢复或上层应用认证；不在主机/私钥失陷后继续保证明文安全。

## 信任边界

1. 网络到 server control/work accept；
2. 外部用户到 server public listener；
3. client 到 local service；
4. 配置、授权、密钥和日志文件到进程；
5. Kconfig/CMake/CI/vendored source 到二进制；
6. 每个已认证 client 身份之间的授权隔离边界。

## 滥用场景

- slow first frame/handshake 占用单线程事件循环；
- 合法 client 提交 work socket 后不回复 READY；
- 重放旧 HELLO/AUTH/WORK/READY/DATA；
- 修改 cipher、client/service/stream/work/session/mode/sequence/header/tag；
- wildcard 与 specific listener 重叠以截获另一 client 的连接；
- 重复配置键、重复身份段、symlink 和权限过宽文件改变信任根；
- 认证失败/注册失败洪泛写满嵌入式存储；
- 编译期裁剪 required DATA 后静默回退明文；
- partial read/write、半关闭、fd 复用和延迟事件触发生命周期错误。

安全审查应同时验证拒绝行为和资源释放，不能把“密码校验失败”视为完整 DoS 防护。

