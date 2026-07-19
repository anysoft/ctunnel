# 链接模式

链接模式是 Kconfig 的三选一配置，并且独立于功能档位和二进制角色。Mini、Default 或 Full 都可以与仅客户端、仅服务端或两种角色组合，也可以选择目标平台支持的任意链接模式。

| Kconfig 符号 | 含义 |
|---|---|
| `CTUNNEL_LINK_DYNAMIC` | 使用平台默认动态 C 运行库。Monocypher 仍编译进 `ctunnel`，适合开发和调试。 |
| `CTUNNEL_LINK_MOSTLY_STATIC` | 把 ctunnel 和 Monocypher 静态库编入可执行文件，同时允许目标 libc/系统运行库保持动态。这是默认模式。 |
| `CTUNNEL_LINK_STATIC` | Linux 请求静态用户态运行库，Windows 请求静态 CRT，macOS 明确失败。 |

Mostly-static 是 Buildroot/OpenWrt/glibc 设备的推荐默认值：它没有第三方密码学共享库依赖，复用设备已有的 libc 和动态加载器，并且通常远小于 glibc 全静态程序。第一期中 Dynamic 与 Mostly-static 可能产生相同的依赖表，因为 Monocypher 始终从源码编译；独立选项用于记录部署意图，也为未来的项目库保留清晰语义，而不会改变功能或角色配置。

## 选择模式

交互或非交互 Kconfig：

```sh
make menuconfig
scripts/config --enable CTUNNEL_LINK_DYNAMIC
scripts/config --enable CTUNNEL_LINK_MOSTLY_STATIC
scripts/config --enable CTUNNEL_LINK_STATIC
```

configure 前端（以下三个参数互斥）：

```sh
./configure --enable-mini --enable-server --enable-mostly-static
./configure --enable-mini --enable-server --enable-static
./configure --enable-default --enable-client --enable-dynamic
```

Make 可以只修改当前 `.config` 中的链接 choice 并重新构建：

```sh
make dynamic
make mostly-static
make static
```

复用 fragment 可避免为每种组合复制完整 defconfig：

```sh
python3 scripts/merge_config.py \
  --config .config --build-dir build --target linux \
  configs/mini_defconfig \
  configs/role/server.config \
  configs/link/static.config
```

`make mini_static_defconfig` 和 `make mini_mostly_static_defconfig` 是这些 fragment 的便捷封装。

## 平台行为

### Linux 平台

全静态模式加入 `-static`。默认 Kconfig 会启用函数/数据独立节和 `--gc-sections`；如果请求 LTO，CMake 会先检测支持情况。生成构建文件前，CMake 会使用目标编译器和 sysroot 链接一个小型可执行文件。目标 SDK 缺少静态 libc 或编译器运行库时，会直接给出说明 SDK 问题的错误，而不是等到最终链接时只显示晦涩的失败信息。

每次最终链接都会执行 `file`、目标 `readelf -l`、目标 `readelf -d`，并在可执行时运行原生 `ldd`。全静态验收要求没有 `INTERP` 程序头和 `NEEDED` 动态项；`ldd` 必须把产物识别为非动态或静态链接。交叉构建会跳过宿主机 `ldd`，因为宿主加载器不应检查或执行目标程序；目标 `readelf` 检查仍然强制执行。

### macOS 平台

macOS 支持 Dynamic 和 Mostly-static。两者可以依赖 `/usr/lib/libSystem.B.dylib`，但 `otool -L` 不能出现第三方密码学 dylib。请求 Fully-static 会在 CMake 配置阶段失败：

```text
Fully static linking is not supported on macOS.
Use mostly-static instead.
```

ctunnel 不会把使用 libSystem 的普通 Mach-O 可执行文件标记为全静态。

### Windows 平台

MSVC 在 Dynamic/Mostly-static 模式使用 `/MD`，在 Static 模式使用 `/MT`。MinGW Static 模式加入 `-static -static-libgcc`，因此 MinGW 支持库不会成为额外 DLL 依赖；具体工具链仍可能通过 Windows 系统 CRT 提供 C API。项目代码和 Monocypher 会编入可执行文件，Static 模式不能依赖非系统的编译器/CRT 或第三方密码学 DLL。Winsock、BCrypt、Kernel32 以及工具链所选 Windows CRT 等系统 DLL 仍可作为平台依赖；“静态”不表示 Windows 系统 DLL 可以被移除。

链接完成后会记录并检查 `dumpbin /dependents` 或 `objdump -p`。

## libc 策略

### glibc 策略

glibc 全静态程序可能明显增大，通常不适合 16 MB Flash，并可能限制 NSS/DNS 行为。即使代码已经静态链接，部分 API 仍会读取运行时系统配置。CMake 会为此组合输出警告。除非经过部署测试证明必须且可正确使用 glibc 全静态产物，否则应优先选择 Mostly-static。

### musl 策略

musl 是全静态参考配置，通常能生成更小、更独立的产物。CI 的 `linux-x86_64-musl-static` 任务会验证不存在 ELF 解释器和任何 `NEEDED` 项。

### uClibc 策略

项目不做无依据的全面兼容承诺，必须测试准确的目标 SDK 和固件运行环境。自动检测不充分时可设置 `-DCTUNNEL_LIBC=uClibc`，使报告保持明确。

每次交叉构建都必须使用目标 SDK 的编译器、sysroot、`ar`、`readelf`、`size` 和静态库。不能用宿主机 libc 填补目标 SDK 中缺失的 `.a` 文件。`-DCTUNNEL_LIBC=<name>` 只给报告增加标签，不会改变实际链接的 sysroot。

## 可审计报告

每个可执行文件旁都会生成 `ctunnel.link-report.txt`，记录链接模式、libc、编译器版本和路径、目标 triplet、文件大小、节表及依赖检查。`scripts/size-report.sh` 会合并同一份元数据，确保比较 Dynamic、Mostly-static 和 Fully-static 的体积时能够看到对应链接策略。

验证器会拒绝所有非静态依赖表中的 libsodium、libssl、libcrypto、libmbedTLS 和外部 Monocypher 共享库。Linux 全静态还会拒绝动态解释器和所有 `NEEDED` 项；Windows Static 会拒绝动态编译器/CRT 运行库。
