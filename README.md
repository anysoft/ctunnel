# ctunnel

`ctunnel` 是一个使用 C11 编写的轻量级反向 TCP 隧道，面向资源受限的 Linux/OpenWrt/Buildroot 设备，同时支持 macOS 和 Windows。项目采用 BusyBox 风格的 Kconfig 构建体系，可生成仅服务端、仅客户端或同时包含两种角色的可执行文件。客户端主动建立经过认证的控制连接及所有工作连接，因此服务端不会反向连接客户端地址。

第一期支持 TCP 映射、多服务、双向公钥认证、临时 X25519、加密并防重放的控制帧、经过认证的工作连接、按服务配置 DATA `required`/`disabled`、心跳、断线重连与重新注册、有界缓冲区，以及 IPv4/IPv6。第一期不实现 UDP、HTTP 路由、TLS 终止、QUIC、多路复用或管理面板。

## 第一期密码学方案

第一期只有一个密码学后端：随源码固定提供的 Monocypher 4.0.3 核心库。项目使用其真实核心 API 和算法：

- 签名：Monocypher EdDSA-BLAKE2b（`crypto_eddsa_*`），不是 Ed25519；
- 临时密钥协商：X25519（`crypto_x25519*`）；
- 认证加密（AEAD）：XChaCha20-Poly1305（`crypto_aead_lock`/`crypto_aead_unlock`）；
- KDF 与握手转录哈希：BLAKE2b 提取及计数器模式扩展；
- 随机数：操作系统 CSPRNG（Linux 使用 `getrandom` 并回退到 `/dev/urandom`，macOS/BSD 使用 `arc4random_buf`，Windows 使用 BCrypt）。

Monocypher 会直接编译进可执行文件。目标系统不需要安装密码学共享库或 pkg-config 包，构建过程也不会从网络下载依赖。源码来源、哈希和许可证记录在 `third_party/monocypher/` 下。

来源与更新规则见 `docs/dependencies.md`，实测文件大小、节、符号和依赖结果见 `docs/size-report.md`。

## 链接模式

链接策略是独立的 Kconfig 三选一配置。默认的 Mostly-static 会把 ctunnel 与 Monocypher 编入同一个可执行文件，同时允许使用目标系统已有的 libc/运行库。Dynamic 使用平台默认动态运行库，适合开发调试。Fully-static 在具备静态库的 Linux SDK 上生成不含动态解释器和 `NEEDED` 项的 ELF；Windows 使用静态 CRT（仍保留系统 DLL）；macOS 会明确拒绝该模式。

```sh
make dynamic
make mostly-static
make static
./configure --enable-mini --enable-server --enable-mostly-static
```

每次链接都会生成经过验证的 `ctunnel.link-report.txt`，其中记录真实依赖表、链接模式、libc、编译器、目标 triplet、文件大小和节信息。fragment 组合方式、musl/glibc/uClibc 策略、平台语义及验证规则见 `docs/link-modes.md`。

## 配置、构建和测试

需要 Python 3.8+、CMake 3.16+ 和 C11 编译器。Kconfiglib 14.1.0 已固定版本并随源码提供，配置阶段不会下载软件包。

```sh
make default_defconfig
make
make test
```

`.config` 是功能配置的唯一来源，并生成 `include/generated/autoconf.h`、`build/generated/config.cmake` 和 `build/generated/config.mk`；CMake 不维护第二套功能选项。可通过以下命令检查或自动修改：

```sh
make menuconfig
scripts/config --enable FEATURE_BUILD_INFO
scripts/config --set-val MAX_STREAMS 64
make olddefconfig
```

Linux 默认使用 epoll，macOS/BSD 默认使用 kqueue，Windows 默认使用 WSAPoll。直接执行 CMake 时，仅在配置文件不存在的情况下创建平台默认 `.config`。预设、非交互构建、`./configure` 映射、硬限制和外部配置路径见 `docs/build-configuration.md`。

## Mini 构建

Mini 是仅支持 IPv6 的服务端程序，编译期限制为一个客户端、四个服务、八个流，使用 Warn 级别日志和小型工作连接池，并保留强制的控制通道认证/加密与工作连接绑定。它裁剪客户端角色、转发 DATA 加密、keygen/fingerprint/configtest/build-info applet、详细帮助，以及 Debug/Trace 日志。密钥应使用功能完整的宿主机构建生成。

```sh
make mini_defconfig
make BUILD_TYPE=MinSizeRel
make size-check
```

`make mini` 会组合执行以上命令。`make size-compare` 构建 Default、Mini 服务端、Mini 客户端和 Full 变体，并保存每份 `.config` 和体积报告。`make dist` 生成已裁剪符号的 `dist/ctunnel`、`dist/ctunnel.config`、链接/体积报告及校验和。

## 子命令（Applet）

程序只会列出并接受已经编译进二进制的 applet：

```sh
ctunnel server -c server.ini
ctunnel client -c client.ini
ctunnel configtest -c client.ini
ctunnel keygen --private client.key --public client.pub
ctunnel fingerprint client.pub
ctunnel build-config
```

当对应角色被编译时，也支持 `ctunnel-server` 和 `ctunnel-client` 这两个 argv[0] 别名。旧式 `ctunnel -c FILE` 调用仍然可用。被禁用的 applet 不会在调度表中出现，其实现目标文件也不会进入二进制。

## 快速开始

在可信主机上分别生成服务端和客户端身份：

```sh
ctunnel keygen --private server.key --public server.pub
ctunnel keygen --private client.key --public client.pub
ctunnel fingerprint server.pub
```

只把 `client.pub` 复制到服务端，只把 `server.pub` 复制到客户端。不要把任一端的私钥复制给另一端，并使用 `0600` 权限保护私钥。可从 `examples/server.ini`、`examples/clients.ini` 和 `examples/client.ini` 开始配置；授权文件必须允许准确的远端监听地址和端口。

```sh
ctunnel configtest -c /etc/ctunnel/server.ini
ctunnel server -c /etc/ctunnel/server.ini
```

如果应用流量已经由正确配置的 SSH 或 TLS 提供端到端保护，可将 DATA 加密设为 `disabled`，避免重复加密。明文 HTTP、Telnet 和数据库流量应使用 `required`。控制通道始终加密且不能关闭。第一期唯一接受的密码套件名称是 `xchacha20-poly1305`，其他值会被拒绝。

## 交叉编译与安全

ARM32 Buildroot、ARMv7、AArch64、MIPS/MIPSEL、OpenWrt 和 MinGW 构建方式见 `docs/cross-compilation.md`。目标 sysroot 只需提供 libc 和网络运行库；Monocypher 使用仓库中固定版本的源码编译。

将监听器暴露到公网前，请阅读 `docs/security.md`。DATA `disabled` 会使网络观察者看到应用明文。本项目使用自定义协议，尚未经过独立安全审计，不应将其描述为 TLS 替代品或绝对安全方案。控制会话丢失时，活动流会被关闭而不是恢复。
