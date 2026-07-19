# Buildroot 与 OpenWrt 集成

两个包示例都会生成项目自己的 `.config`，并通过 `CTUNNEL_KCONFIG_CONFIG` 传入其路径。它们不会把 ctunnel 无前缀的生成宏 `CONFIG_*` 暴露给外层构建，也不会选择 libsodium、OpenSSL 或 mbedTLS。

两者都继承 ctunnel 的 Mostly-static 默认值：Monocypher 和项目代码位于可执行文件中，固件 libc/运行库可以保持动态。只有在准确的目标 SDK 提供完整静态 libc/运行库时才使用 `configs/link/static.config`。Buildroot glibc 镜像通常应保持 Mostly-static；musl 是全静态首选目标；uClibc 必须针对实际 SDK 测试。

## Buildroot 集成

`packaging/buildroot/ctunnel/Config.in` 会在预配置 hook 中把 `BR2_PACKAGE_CTUNNEL_MINI`、`BR2_PACKAGE_CTUNNEL_SERVER_ONLY`、`BR2_PACKAGE_CTUNNEL_IPV6_ONLY` 及三选一链接模式映射到项目配置。生成文件位于包构建目录。`host-python3` 只是离线 Kconfig 前端所需的宿主机构建依赖。

将 `packaging/buildroot/ctunnel` 复制到外部树或对应 Buildroot 包目录，并引入其 `Config.in`。由于 Monocypher 从内置 C 源码编译，ctunnel 目标仍只依赖 libc/套接字运行库。

## OpenWrt 集成

`packaging/openwrt/Makefile` 使用同一源码树定义三个变体：

- `ctunnel-mini-server` 使用 `configs/openwrt_mini_defconfig`；
- `ctunnel-client` 使用 `configs/mini_client_defconfig`；
- `ctunnel-full` 使用 `configs/full_defconfig`。

软件包声明 `python3/host`，在配置阶段生成 `$(PKG_BUILD_DIR)/.config`，再交给标准 OpenWrt CMake helper 使用。变体定义不会复制或分叉源码。

包配方中的 `CTUNNEL_LINK_SYMBOL` 默认为 `CTUNNEL_LINK_MOSTLY_STATIC`。只有确认 OpenWrt SDK 提供全部静态运行库后，目标 feed 才可以把它覆盖为 `CTUNNEL_LINK_STATIC`。

在任一生态中，都应随固件构建产物保存最终 `.config`、已裁剪符号的二进制、`ctunnel.link-report.txt`、体积报告、架构/解释器报告、动态依赖列表和校验和。链接报告记录 libc、编译器、triplet、链接模式、节和文件大小，可防止把 Mostly-static 结果误标为 Fully-static。
