# 系统架构

服务端运行在公网可访问设备上，客户端运行在私有服务一侧，并主动发起控制连接和每一条工作连接。服务端不会解析或连接客户端地址。

进程采用单线程模型。Linux 使用 epoll，macOS/BSD 使用 kqueue，可移植 POSIX 构建可使用 poll，Windows 使用 WSAPoll。Kconfig 在编译时只选择一个后端。非阻塞套接字、可选的 TCP keepalive 调优、有界等待队列和编译期定长的转发环形缓冲区共同提供背压，避免流缓冲区无限增长。

```text
配置/应用层
        |
        +-- 协议 + 网络 + 转发状态机
        |          |
        |          +-- include/ctunnel/crypto.h
        |                    |
        |                    +-- src/crypto/crypto_monocypher.c
        |                              |
        |                              +-- 随源码提供的 Monocypher 4.0.3 核心
        |
        +-- 平台事件循环与操作系统 CSPRNG
```

可执行文件入口是一张编译期 applet 表。server、client、keygen、fingerprint、configtest 和 build-info 的实现位于不同翻译单元；CMake 不会加入已禁用的单元。`src/app/runtime.c` 只包含共享的进程停止处理逻辑。

只有 `src/crypto/crypto_monocypher.c` 会包含 `monocypher.h`；协议、网络、转发、密钥工具和应用代码都依赖公开的密码学抽象层。

双向认证完成后，客户端注册 TCP 服务。服务端会先应用该客户端专属的地址、端口、服务数和流数量策略，再打开监听器。加密控制消息不携带应用 DATA。

客户端维护空闲工作连接池。每条工作连接包含经过认证的单调递增序号和新鲜随机字节；服务端使用 64 项滑动窗口防止重放。外部连接到达后会消耗一条工作连接，分配新的流 ID 和 32 字节连接随机数，并发送经过认证的流元数据。客户端连接其本地目标端点，回复经过认证的就绪结果，然后补充连接池。连接池大小为零时，系统使用有界等待队列和 `REQUEST_WORK_CONNECTION`。

DATA `disabled` 模式通过有界环形缓冲区直接复制原始字节。DATA `required` 模式使用每流、每方向密钥和单调递增序号编码独立 AEAD 记录。转发状态机能够处理部分读写、`EAGAIN`、`EINTR`、半关闭和慢速写端。

心跳使用加密 PING/PONG 帧。超时会销毁监听器、等待/工作套接字和活动转发。客户端使用指数退避和操作系统随机抖动重连，重新执行临时密钥握手、注册服务并重建连接池。旧会话密钥、nonce 基值、ID 和流均不会复用。
