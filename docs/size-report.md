# 体积报告

以下数据于 2026-07-19 在 macOS arm64、AppleClang 21.0.0 上测得。Mini 使用 `MinSizeRel`、`CONFIG_OPTIMIZE_FOR_SIZE=y`、LTO、函数/数据独立节、dead stripping、内置 Monocypher、仅 IPv6、Warn 日志，并禁用 DATA 转发加密和可选工具/报告 applet。数值单位均为文件字节。

## 角色与档位对比

| 变体 | 角色 | 链接模式 | 未裁剪 | 已裁剪 | 相对 Default |
|---|---|---|---:|---:|---:|
| Default | 客户端 + 服务端 | Mostly-static | 141,528 | 135,760 | 0 |
| Mini 服务端 | 仅服务端 | Mostly-static | 107,048 | 102,480 | -33,280 |
| Mini 客户端 | 仅客户端 | Mostly-static | 90,296 | 85,824 | -49,936 |
| Full Debug | 客户端 + 服务端 | Mostly-static | 201,400 | 169,152 | +33,392 |

上述角色对比也改变了文档所述档位功能，并不是只改变角色的隔离基准。使用 `scripts/size-compare.sh` 可重新生成。脚本会把每个解析后的 `.config`、二进制和完整报告保存在 `build/size-comparison/` 下，使功能差异可审计。

Mini 服务端的 Mach-O 节为 `__text` 39,472 字节、`__const` 3,748 字节、`__cstring` 3,250 字节；对齐后的文件 segment 占据 102,480 字节已裁剪文件的大部分空间。Mini 客户端的 `__text` 为 37,436 字节、`__const` 为 3,740 字节、`__cstring` 为 2,992 字节。两者都低于配置的 400 KiB 已裁剪上限。

## 裁剪证据

`scripts/check-binary-config.sh` 会检查 `nm`、`strings` 和平台动态依赖表。实测 Mini 服务端不包含客户端入口/别名、keygen、fingerprint、build-info/build-config、DATA 密钥派生符号/上下文或 Debug/Trace 字符串。Mini 客户端不包含服务端入口/别名，同样不包含上述可选符号和字符串。`scripts/check-runtime-config.sh` 确认程序会拒绝相反角色配置及未编译的地址族/DATA 能力。

控制通道 AEAD 符号会有意保留：禁用 `CONFIG_FEATURE_DATA_ENCRYPTION` 只会移除二次应用 DATA 记录。认证、加密并防重放的控制帧、工作连接绑定、随机数失败检查、硬限制以及 Monocypher 验证/擦除函数始终强制保留。

## 动态依赖

四个 macOS arm64 变体都只列出 `/usr/lib/libSystem.B.dylib`，没有链接 libsodium、OpenSSL/libcrypto 或 mbedTLS。它们属于 Mostly-static 而不是 Fully-static，因为 macOS 不能静态链接通用系统运行库。Windows 还会链接 Winsock 和 BCrypt 等系统 DLL。Linux Mostly-static 依赖所选目标 libc/加载器；Monocypher 编译进 ctunnel。

现在每份报告开头都会记录 `link_mode`、`libc`、`compiler` 和 `target_triplet`，相邻的链接后报告还包含文件大小、节数据和完整依赖检查。即使两个模式在某个平台恰好生成相同字节，Dynamic、Mostly-static 和 Fully-static 的测量值也必须放在不同记录中。

## 交叉目标状态

本 macOS 主机没有对应 SDK/交叉编译器，因此无法准确测量 Buildroot ARM32、Linux ARM64、ARMv7、MIPS 和 MIPSEL 的已裁剪体积；文档不会编造数据。GitHub Actions 交叉矩阵会为每个已安装交叉编译器生成 Mini 服务端 `.config`、链接报告、架构和依赖输出、目标 strip 体积报告、校验和及二进制。原生矩阵分别记录 glibc Dynamic、glibc Mostly-static 和 musl Fully-static；ARMv7 同时覆盖 Mostly-static 和依赖 SDK 能力的 Fully-static。

## 隔离功能差值

同一脚本还会构建除单项变化外均保持 Default 功能、Default 限制、两种角色、`MinSizeRel+LTO` 的配置。基线为未裁剪 108,648 字节、已裁剪 102,688 字节。

| 隔离变化 | 未裁剪 | 差值 | 已裁剪 | 差值 |
|---|---:|---:|---:|---:|
| 仅服务端 | 108,328 | -320 | 102,688 | 0 |
| 仅客户端 | 108,072 | -576 | 102,528 | -160 |
| 关闭 keygen（保留 fingerprint） | 108,248 | -400 | 102,512 | -176 |
| 最高 Warn 日志 | 108,584 | -64 | 102,688 | 0 |
| 最高 Debug 日志 | 108,664 | +16 | 102,688 | 0 |
| 最高 Trace 日志 | 108,696 | +48 | 102,688 | 0 |
| 关闭 DATA 转发加密 | 108,408 | -240 | 102,688 | 0 |
| 关闭工作连接池 | 108,648 | 0 | 102,688 | 0 |
| 关闭 build-info | 108,440 | -208 | 102,688 | 0 |

Mach-O segment/link-edit 对齐会在整个已裁剪文件粒度上隐藏若干较小的代码/字符串差值。符号和字符串检查仍能证明它们已移除，未裁剪文件值也能显示部分差异。当隔离变化仍落在同一对齐 segment 大小时，应优先比较链接器 map 或节级数据。可通过以下命令复现受控变体：

```sh
scripts/config --disable FEATURE_KEYGEN --disable FEATURE_FINGERPRINT
scripts/config --disable FEATURE_DATA_ENCRYPTION
scripts/config --enable LOG_MAX_LEVEL_WARN
make BUILD_DIR=build/variant BUILD_TYPE=MinSizeRel size-report
```

后续体积优化应优先分离仅握手使用的密码学代码与仅工具使用的格式化代码，评估面向固定固件镜像的更小配置解析器，以结构化数字诊断减少帮助/错误文本，并在各目标上测量 `poll` 与 epoll/kqueue。不得移除认证、控制通道 AEAD、熵源错误处理、验证或边界检查。
