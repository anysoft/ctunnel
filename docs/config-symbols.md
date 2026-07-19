# Kconfig 符号目录

这些符号在 C 代码中表现为 `CONFIG_<name>`。隐藏符号由 choice 或目标环境生成，通常不应直接编辑。

## 目标、角色和构建

| 符号 | 含义 / 依赖 |
|---|---|
| `CTUNNEL_TARGET_OS`、`TARGET_LINUX`、`TARGET_DARWIN`、`TARGET_BSD`、`TARGET_WINDOWS` | 由配置前端提供的隐藏宿主机/目标值。 |
| `CTUNNEL_ROLE_BOTH`、`CTUNNEL_ROLE_CLIENT_ONLY`、`CTUNNEL_ROLE_SERVER_ONLY` | 角色三选一。 |
| `CTUNNEL_CLIENT`、`CTUNNEL_SERVER` | 由角色 choice 推导出的隐藏值，用于驱动 CMake 源码选择。 |
| `OPTIMIZE_FOR_SIZE` | 增加体积优化编译选项，并使用更小的 Monocypher BLAKE2 代码。 |
| `ENABLE_LTO` | 请求编译器/链接器执行跨过程优化；不支持时配置失败。 |
| `ENABLE_SECTION_GC` | 启用函数/数据独立节及平台 dead-strip/GC。 |
| `CTUNNEL_LINK_DYNAMIC`、`CTUNNEL_LINK_MOSTLY_STATIC`、`CTUNNEL_LINK_STATIC` | 独立于档位和角色的链接模式三选一；默认 Mostly-static。 |

## 协议和网络能力

| 符号 | 含义 / 依赖 |
|---|---|
| `FEATURE_TCP` | 第一期必需的转发协议。 |
| `FEATURE_IPV4`、`FEATURE_IPV6` | 至少启用一个；程序会拒绝已禁用地址族的数字地址。 |
| `FEATURE_WORK_CONNECTION` | 第一期必需的已认证工作连接。 |
| `FEATURE_WORK_POOL` | 可选空闲连接池，依赖 `FEATURE_WORK_CONNECTION`；禁用后池默认值和运行时值必须为零。 |
| `FEATURE_CLIENT_RECONNECT` | 仅客户端的自动重连循环，依赖客户端角色菜单。 |

## 密码学

| 符号 | 含义 / 依赖 |
|---|---|
| `CRYPTO_MONOCYPHER` | 第一期唯一且强制的密码学后端。 |
| `CRYPTO_CHACHA20_POLY1305` | 强制的控制通道 XChaCha20-Poly1305，依赖 Monocypher。 |
| `FEATURE_PUBLIC_KEY_AUTH` | 强制的 EdDSA-BLAKE2b 对端认证，依赖 Monocypher。 |
| `FEATURE_DATA_ENCRYPTION` | 可选的二次转发 DATA 记录加密，依赖 AEAD 实现；该符号不控制且不能关闭控制通道加密。 |

## 子命令（Applet）和配置

| 符号 | 含义 / 依赖 |
|---|---|
| `FEATURE_KEYGEN` | 加入 keygen 翻译单元/applet，依赖 Monocypher。 |
| `FEATURE_FINGERPRINT` | 加入 fingerprint 翻译单元/applet，依赖 Monocypher。 |
| `FEATURE_CONFIG_TEST` | 加入 configtest applet。 |
| `FEATURE_BUILD_INFO` | 加入 build-info 和 build-config applet。 |
| `FEATURE_VERBOSE_HELP` | 加入详细帮助和别名文本。 |
| `FEATURE_INI_CONFIG` | 第一期强制的运行时配置解析器。 |
| `FEATURE_ENV_OVERRIDE` | 启用 `CTUNNEL_LOG_LEVEL` 和 `CTUNNEL_POOL_COUNT` 环境覆盖。 |
| `FEATURE_COMMAND_LINE_OVERRIDE` | 启用角色 applet 的 `--log-level` 参数。 |
| `FEATURE_UNKNOWN_CONFIG_ERROR` | 强制对未知配置键报错。 |

## 日志、平台和诊断

| 符号 | 含义 / 依赖 |
|---|---|
| `LOG_MAX_LEVEL_ERROR`、`LOG_MAX_LEVEL_WARN`、`LOG_MAX_LEVEL_INFO`、`LOG_MAX_LEVEL_DEBUG`、`LOG_MAX_LEVEL_TRACE` | 编译期最高日志级别五选一。 |
| `LOG_ERROR`、`LOG_WARN`、`LOG_INFO`、`LOG_DEBUG`、`LOG_TRACE` | 推导出的隐藏逐级宏；禁用的调用及其参数会在预处理时消失。 |
| `EVENT_EPOLL` | 事件实现，仅 Linux。 |
| `EVENT_KQUEUE` | 事件实现，仅 Darwin/BSD。 |
| `EVENT_POLL` | 事件实现，Windows 以外的 POSIX。 |
| `EVENT_WSAPOLL` | 事件实现，仅 Windows。 |
| `FEATURE_TCP_KEEPALIVE_TUNING` | 启用套接字 keepalive 配置。 |
| `FEATURE_SIGNAL_HANDLING` | 在支持的平台启用进程信号处理。 |
| `FEATURE_ASSERTIONS` | 启用内部环形缓冲区/状态断言。 |
| `FEATURE_PROTOCOL_DIAGNOSTICS` | 加入协议诊断源码和更详细的拒绝日志。 |
| `FEATURE_TEST_HOOKS` | 加入显式的测试专用运行时 hook 符号。 |

## 整数限制和默认值

| 符号 | 范围 / 关系 |
|---|---|
| `MAX_CLIENTS` | 1..64 |
| `MAX_SERVICES` | 1..256 |
| `MAX_STREAMS` | 1..4096 |
| `MAX_PENDING_STREAMS` | 1..1024 |
| `MAX_FRAME_SIZE` | 4096..1048576 |
| `MAX_CLIENT_ID_LENGTH`、`MAX_SERVICE_ID_LENGTH` | 8..256 |
| `MAX_ADDRESS_LENGTH` | 16..512 |
| `MAX_PATH_LENGTH` | 64..2048 |
| `MAX_PORT_RANGES` | 1..128 |
| `STREAM_BUFFER_SIZE` | 4096..262144 |
| `CONTROL_BUFFER_SIZE` | 256..65536 |
| `DEFAULT_MAX_STREAMS` | 1..`MAX_STREAMS` |
| `DEFAULT_MAX_PENDING_STREAMS` | 1..`MAX_PENDING_STREAMS` |
| `DEFAULT_POOL_MIN`、`DEFAULT_POOL_COUNT`、`DEFAULT_POOL_MAX` | 0..64，并验证 min <= count <= max；连接池禁用时三者都必须为零。 |
| `MINI_MAX_BINARY_SIZE_KIB` | 64..2048，供 size-check 使用，默认 400。 |

Kconfig 范围负责检查单个值。`scripts/kconfig/ctkconfig.py` 还会在生成任何构建文件前验证第一期强制安全功能、角色/IP 可用性、跨符号默认值与最大值关系、连接池顺序以及目标/事件后端兼容性。
