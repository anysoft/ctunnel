# ctunnel

`ctunnel` 是一个纯 C11 编写的轻量级反向 TCP 隧道工具，适合把内网设备上的 TCP 服务暴露到一台公网服务器上。客户端主动连到服务端，服务端不会反向连接客户端地址；控制通道和工作连接都会做双向公钥认证，Monocypher 会随源码直接编译进程序，不依赖 libsodium、OpenSSL 或 mbedTLS。

项目重点是小体积、可裁剪、便于交叉编译。它支持 Linux/OpenWrt/Buildroot、macOS 和 Windows，并提供 BusyBox 风格的 Kconfig 配置体系，可以按目标设备选择完整工具集或 Mini 服务端。

## 主要能力

- TCP 反向映射：把客户端本地服务映射到服务端监听端口。
- 多服务配置：一个客户端可注册多个 TCP 服务。
- 公钥身份认证：服务端和客户端都使用固定公钥验证对端。
- 控制通道加密：控制帧始终认证加密并防重放。
- DATA 加密可选：每个服务可设置 `data_encryption = true` 或 `false`。
- 自动重连：客户端断线后自动重连并重新注册服务。
- 可裁剪构建：可选择 client/server/both、Mini/default/full、动态/半静态/全静态链接。

更细的协议和安全说明见 [docs/protocol.md](docs/protocol.md) 和 [docs/security.md](docs/security.md)。

## 下载哪个包

普通桌面/服务器平台优先下载不带 `mini` 的包，例如：

- `linux-x86_64-mostly-static`
- `linux-i686-mostly-static`
- `macos-arm64-mostly-static`
- `macos-x86_64-mostly-static`
- `windows-x86_64-static-runtime`
- `windows-i686-static-runtime`

这些包包含 `client`、`server`、`keygen`、`fingerprint`、`configtest` 和 `build-info` 等常用命令。

ARM、MIPS 等嵌入式设备通常下载 `mini` 包。Mini 包主要用于只跑服务端的小设备，会裁剪 keygen、fingerprint、build-info 等工具；密钥建议在 PC/Mac/Linux 主机上生成后再复制到设备。

Mini 包还有更小的编译期资源上限：通常只允许 `max_clients = 1`、`max_services_per_client = 4`、`max_streams_per_client = 8`、`max_pending_streams = 4`，最高日志级别为 `warn`。运行时配置不能超过这些上限；Release 包内的 `ctunnel.config` 记录了该二进制的实际编译配置。

如果设备是旧 glibc，例如 Buildroot glibc 2.26，不要使用公开 GitHub runner 生成的 ARM mostly-static 包；应使用设备 SDK 本地构建。细节见 [docs/cross-compilation.md](docs/cross-compilation.md)。

## 快速开始

在可信主机上生成两组身份密钥：

```sh
ctunnel keygen --private server.key --public server.pub
ctunnel keygen --private client.key --public client.pub
```

文件用途：

- 服务端保存 `server.key`，并在 `clients.ini` 中引用 `client.pub`。
- 客户端保存 `client.key`，并在 `client.ini` 中引用 `server.pub`。
- 私钥不要复制给对端；Linux/macOS 上建议权限为 `0600`。

服务端配置可从 [examples/server.ini](examples/server.ini) 和 [examples/clients.ini](examples/clients.ini) 开始：

```ini
[common]
mode = server
bind_addr = *
bind_port = 7000
identity_private_key = /etc/ctunnel/server.key
authorized_clients_file = /etc/ctunnel/clients.ini
```

客户端配置可从 [examples/client.ini](examples/client.ini) 开始。下面示例把客户端本机 SSH 映射到服务端的 `2222` 端口：

```ini
[common]
mode = client
server_addr = 2001:db8::10
server_port = 7000
client_id = vps-sg
identity_private_key = /etc/ctunnel/client.key
server_public_key = /etc/ctunnel/server.pub

[ssh]
type = tcp
remote_addr = *
remote_port = 2222
local_addr = 127.0.0.1
local_port = 22
data_encryption = false
```

`bind_addr = *` 和 `remote_addr = *` 表示同时监听 IPv4 wildcard 与 IPv6 wildcard；如果二进制只编译了其中一种地址族，就使用可用的那一种。只想绑定单协议族时可以明确写 `0.0.0.0` 或 `::`。

启动前建议先检查配置：

```sh
ctunnel configtest -c /etc/ctunnel/server.ini
ctunnel configtest -c /etc/ctunnel/client.ini
```

分别启动服务端和客户端：

```sh
ctunnel server -c /etc/ctunnel/server.ini
ctunnel client -c /etc/ctunnel/client.ini
```

然后从外部访问服务端：

```sh
ssh -p 2222 user@server.example.com
```

## 常用命令

```sh
ctunnel server -c server.ini
ctunnel client -c client.ini
ctunnel configtest -c client.ini
ctunnel keygen --private client.key --public client.pub
ctunnel fingerprint client.pub
ctunnel build-info
ctunnel --version
```

实际可用命令取决于编译配置。Mini 包可能只包含 `server`、`--help` 和 `--version`。

默认日志会同时输出到终端和配置文件同目录：服务端为 `ctunnel-server.log`，客户端为 `ctunnel-client.log`。可在 `[common]` 中设置 `log_file` 和 `log_rotate_days` 调整日志文件位置和保留天数。

## 从源码构建

依赖：

- Python 3.8+
- CMake 3.16+
- C11 编译器

默认构建：

```sh
make default_defconfig
make
make test
```

Mini 服务端构建：

```sh
make mini_defconfig
make BUILD_TYPE=MinSizeRel
make size-check
```

也可以使用 `./configure`：

```sh
./configure --enable-mini --enable-server --enable-mostly-static --disable-tests
make
```

更多构建选项见 [docs/build-configuration.md](docs/build-configuration.md)。

## 交叉编译

Buildroot ARM32 示例：

```sh
export CTUNNEL_TOOLCHAIN_ROOT=/path/to/arm-buildroot-linux-gnueabi_sdk-buildroot

./configure \
  --enable-mini \
  --enable-server \
  --enable-mostly-static \
  --with-toolchain-root="$CTUNNEL_TOOLCHAIN_ROOT" \
  --with-toolchain-file=cmake/toolchains/buildroot-arm.cmake.example \
  --with-bundled-monocypher \
  --disable-tests \
  --build-dir=build-arm

cmake --build build-arm --parallel
```

交叉编译、OpenWrt/Buildroot 集成、glibc/musl/uClibc 链接策略见：

- [docs/cross-compilation.md](docs/cross-compilation.md)
- [docs/embedded-builds.md](docs/embedded-builds.md)
- [docs/link-modes.md](docs/link-modes.md)

## 文档索引

- [docs/configuration.md](docs/configuration.md)：运行时配置文件说明。
- [docs/build-configuration.md](docs/build-configuration.md)：Kconfig、configure、Makefile 和 CMake 配置。
- [docs/cross-compilation.md](docs/cross-compilation.md)：交叉编译和 SDK 注意事项。
- [docs/link-modes.md](docs/link-modes.md)：dynamic、mostly-static、fully-static 语义。
- [docs/security.md](docs/security.md)：安全边界和部署注意事项。
- [docs/protocol.md](docs/protocol.md)：线协议和密码学细节。
- [docs/release.md](docs/release.md)：Release artifact 命名和发布规则。

## 安全提醒

`data_encryption = false` 只适合应用层已经有 SSH/TLS 等端到端保护的服务。明文 HTTP、Telnet、数据库协议等应使用 `true`。控制通道始终加密且不能关闭。

ctunnel 使用自定义协议，尚未经过独立安全审计。请不要把它描述为 TLS 的替代品，也不要在高风险场景里只依赖默认示例配置。
