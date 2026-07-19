# 构建配置

ctunnel 使用 BusyBox 风格的 Kconfig 流程。`.config` 是所有功能决策的唯一来源。项目固定并随源码提供 Kconfiglib 14.1.0，使用 Python 3.8 或更高版本即可离线配置。

## 生成文件

每条配置命令都会检查依赖关系和整数范围，然后写出：

- `.config`：用户选择并经解析后的 Kconfig 状态；
- `include/generated/autoconf.h`：C 预处理器能力开关和硬限制；
- `build/generated/config.cmake`：供 CMake 选择源码的值；
- `build/generated/config.mk`：供 Make 和报告脚本使用的值。

CMake 会在构建目录中生成独立的 `autoconf.h` 副本，并将其放在头文件搜索路径最前面。因此，不同配置可以并行构建，不会误用其他构建目录的头文件。Make/配置命令仍会生成源码树中的头文件，供工具使用并保留约定路径。

## 预设和交互配置

```sh
make mini_defconfig       # 仅 IPv6、仅服务端、最小功能集
make mini_static_defconfig
make mini_mostly_static_defconfig
make default_defconfig    # 同时包含两种角色及完整的第一期运行时功能
make full_defconfig       # 开发用限制、诊断、断言和 Debug 日志
make menuconfig
make oldconfig
make olddefconfig
make savedefconfig        # 写入 ./defconfig
```

平台预设包括 `linux_defconfig`、`macos_defconfig`、`windows_defconfig`、`buildroot_defconfig` 和 `openwrt_defconfig`。平台 choice 会且只会选择一个事件实现。无效组合（例如 Windows 上使用 epoll）会在配置阶段失败。

功能档位和角色不会决定链接策略；独立的默认链接模式是 Mostly-static。链接 fragment 位于 `configs/link/`，角色 fragment 位于 `configs/role/`，`scripts/merge_config.py` 按命令行顺序叠加它们。具体语义和平台验证见 `link-modes.md`。

非交互编辑器接受带或不带 `CONFIG_` 前缀的符号名：

```sh
scripts/config --enable CTUNNEL_ROLE_CLIENT_ONLY
scripts/config --disable FEATURE_DATA_ENCRYPTION
scripts/config --set-val MAX_SERVICES 4
scripts/config --state MAX_SERVICES
make olddefconfig
```

每次修改后都会执行 Kconfig choice 和依赖规则。不可能实现的请求会直接失败，而不会被静默改写。

## configure 兼容前端

`./configure` 直接修改 `.config`，不会绕过 Kconfig 传递功能决策。它支持档位、角色、IP 协议族、DATA 加密、工作连接池、keygen、最高日志级别、资源限制、交叉工具链、sysroot、安装前缀、LTO，以及三选一链接模式。

```sh
./configure \
  --enable-mini \
  --enable-server --disable-client \
  --enable-ipv6 --disable-ipv4 \
  --disable-data-encryption --disable-keygen \
  --enable-mostly-static \
  --with-log-level=warn \
  --with-max-services=4
make
```

相互冲突的档位、同时禁用两种角色、同时禁用两种地址族、无效日志级别、不可满足的功能依赖和超范围整数都会导致失败。构建路径和工具链参数单独记录在 `.ctunnel-configure.mk` 中；该文件不保存功能配置。

## 源码级裁剪

CMake 只读取生成的 `config.cmake`，并按条件加入：

- `src/server/server.c` 和服务端 applet；
- `src/client/client.c` 和客户端 applet；
- keygen、fingerprint、configtest 和 build-info applet 的翻译单元；
- epoll、kqueue、poll 或 WSAPoll 中的一个实现。

DATA 转发加密和编译期日志级别也会在共享模块内部进行条件编译。禁用的日志宏展开为 `((void)0)`，因此参数和格式字符串都不会求值或保留。`scripts/check-binary-config.sh` 会检查符号、applet/日志级别字符串以及外部密码学依赖。

## 编译期硬限制

下列值决定固定配置结构的大小，并限制运行时分配或协议输入：

```text
CONFIG_MAX_CLIENTS
CONFIG_MAX_SERVICES
CONFIG_MAX_STREAMS
CONFIG_MAX_PENDING_STREAMS
CONFIG_MAX_FRAME_SIZE
CONFIG_MAX_CLIENT_ID_LENGTH
CONFIG_MAX_SERVICE_ID_LENGTH
CONFIG_MAX_ADDRESS_LENGTH
CONFIG_MAX_PATH_LENGTH
CONFIG_MAX_PORT_RANGES
CONFIG_STREAM_BUFFER_SIZE
CONFIG_CONTROL_BUFFER_SIZE
```

运行时 INI 值可以更小，但不能更大。ctunnel 会报告并拒绝超限值，不会截断。仅 IPv6 构建会拒绝数字形式的 IPv4 地址；单角色构建会拒绝相反模式；禁用 DATA 加密的构建会拒绝 `required`；未编译的日志级别会被拒绝；禁用工作连接池的构建会拒绝非零池大小。

## 部署示例

最小嵌入式服务端：

```sh
make mini_defconfig
make BUILD_TYPE=MinSizeRel
make size-check
make dist
```

小型 VPS 仅客户端二进制：

```sh
python3 scripts/kconfig/configure.py --config .config --build-dir build \
  defconfig configs/mini_client_defconfig
make BUILD_TYPE=MinSizeRel
```

不污染源码树的外部 Buildroot/OpenWrt 配置：

```sh
python3 scripts/kconfig/configure.py \
  --config "$BUILD_DIR/ctunnel.config" \
  --build-dir "$BUILD_DIR/ctunnel-build" \
  --target linux defconfig configs/buildroot_defconfig
cmake -S . -B "$BUILD_DIR/ctunnel-build" \
  -DCTUNNEL_KCONFIG_CONFIG="$BUILD_DIR/ctunnel.config"
```

清理生成状态：

```sh
make distclean
```

该命令只删除约定的 `build`、`dist`、`.config`、源码树生成头文件和 configure 元数据路径。已保存的 `defconfig` 和自定义构建目录会被保留。

## 构建模式与功能配置

Sanitizer 仍是 CMake 编译器模式（`CTUNNEL_ENABLE_ASAN`、`CTUNNEL_ENABLE_UBSAN`），不是目标功能。角色、协议、密码学能力、工具、事件后端、日志上限、资源限制、体积优化、LTO、节回收和链接模式均为 Kconfig 值。链接模式同时独立于角色和功能档位。节回收默认启用；Mini 还使用 `-Os`、LTO 以及体积更小的 Monocypher BLAKE2 实现。
