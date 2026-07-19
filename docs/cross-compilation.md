# 交叉编译

Monocypher 4.0.3 以 C 源码形式随项目提供，并由所选目标编译器编译。交叉构建不会发现、下载或链接宿主机/目标系统的密码学软件包。目标 sysroot 仍需提供 libc、套接字运行库，以及除 Linux 全静态外所需的动态加载器。工具链文件不会执行目标程序。全静态请求会使用目标编译器进行一次编译和链接能力探测，但不会运行探测结果。

## Buildroot ARM32 / glibc 2.26 交叉构建

```sh
export CTUNNEL_TOOLCHAIN_ROOT=/root/jonhy/toolchains/arm-buildroot-linux-gnueabi_sdk-buildroot
python3 scripts/kconfig/configure.py --config build-arm/.config --build-dir build-arm \
  --target linux defconfig configs/buildroot_defconfig
scripts/config --file build-arm/.config --build-dir build-arm --target linux \
  --enable CTUNNEL_LINK_MOSTLY_STATIC
cmake -S . -B build-arm \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/buildroot-arm.cmake.example \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCTUNNEL_KCONFIG_CONFIG=build-arm/.config \
  -DCTUNNEL_USE_BUNDLED_MONOCYPHER=ON
cmake --build build-arm --parallel
```

使用同一个 SDK 中的工具验证产物：

```sh
file build-arm/ctunnel
$CTUNNEL_TOOLCHAIN_ROOT/bin/arm-buildroot-linux-gnueabi-readelf -h build-arm/ctunnel
$CTUNNEL_TOOLCHAIN_ROOT/bin/arm-buildroot-linux-gnueabi-readelf -l build-arm/ctunnel
$CTUNNEL_TOOLCHAIN_ROOT/bin/arm-buildroot-linux-gnueabi-readelf -d build-arm/ctunnel
CTUNNEL_STRIP=$CTUNNEL_TOOLCHAIN_ROOT/bin/arm-buildroot-linux-gnueabi-strip \
  scripts/size-report.sh build-arm/ctunnel
```

模板要求在指定位置恰好找到一个预期编译器，使用 SDK sysroot，选择同一 SDK 的 `ar`、`ranlib`、`strip`、`readelf`、`objdump` 和 `size`，并把库、头文件和包搜索限制在目标根目录。设备已有兼容 glibc 2.26 时应选择 Mostly-static；使用宿主机较新 glibc 链接的二进制并不兼容，而 glibc 全静态会显著增加体积并影响 NSS/DNS 行为。

公开 GitHub Actions 的 `arm-linux-gnueabi` 工具链来自当前 Ubuntu 发行版，生成的 Mostly-static 二进制会记录较新的 glibc 版本需求，例如 `GLIBC_2.34`。这类产物不能部署到 glibc 2.26 设备。面向该设备的 Mostly-static 包必须用上面的 Buildroot SDK 构建，或在安装了同一 SDK 的私有 runner 上构建；公开 Release 只发布 ARMv5 全静态参考包，避免把新 glibc 产物误认为 Buildroot/glibc 2.26 兼容包。

也可使用封装前端：

```sh
./configure --enable-mini \
  --enable-mostly-static \
  --with-toolchain-root="$CTUNNEL_TOOLCHAIN_ROOT" \
  --with-toolchain-file=cmake/toolchains/buildroot-arm.cmake.example \
  --with-bundled-monocypher \
  --build-dir=build-arm
cmake --build build-arm --parallel
```

## 其他目标

工具链模板覆盖 i686、ARMv5 gnueabi 软浮点、ARMv7 gnueabihf 硬浮点、AArch64、MIPS、MIPSEL 和 MinGW。编译器前缀不同时设置 `CTUNNEL_CROSS_PREFIX`。MIPS 大小端由编译器决定（`mips-...` 与 `mipsel-...`）。ARMv5 软浮点使用 `arm-linux-gnueabi` 工具链，默认编译参数为 `-march=armv5te -mfloat-abi=soft`，适合不支持 hard-float ABI 的旧 ARM 设备；不要用 `arm-linux-gnueabihf` 产物替代。每次构建后都应检查架构、解释器、`NEEDED` 项和体积；二进制不能列出外部密码学库。ARM/Buildroot 全静态构建只有在该 SDK 提供所有目标 `.a` 文件时才有效，绝不能混用宿主机和目标机 libc 库。

OpenWrt 和 Buildroot 映射见 `docs/embedded-builds.md`。它们启用内置 Monocypher，不依赖密码学软件包，并把 `.config` 保存在包构建目录。MSVC 和 MinGW 编译同一份内置源码，不需要 vcpkg 包。MinGW Static 表示编译器支持运行库采用静态方式，Windows 系统 DLL 仍然存在。macOS x86_64 和 arm64 应分别构建，仅当 `.config` 完全相同时才可合并；macOS 不支持通用全静态系统程序。
