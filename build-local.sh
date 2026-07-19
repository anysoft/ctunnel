#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
用法:
  ./build-local.sh [选项]

默认行为:
  - 当前平台自动选择 Kconfig target 和 defconfig
  - macOS 默认使用 configs/macos_defconfig
  - Linux 默认使用 configs/default_defconfig
  - 默认链接模式 mostly-static
  - 默认构建类型 Release
  - 默认使用 bundled Monocypher
  - 默认打包 release tar.gz/zip

常用示例:
  ./build-local.sh
  ./build-local.sh --type Debug --tests
  ./build-local.sh --link dynamic --no-package
  ./build-local.sh --defconfig configs/mini_defconfig --type MinSizeRel

交叉编译示例:
  CTUNNEL_CROSS_PREFIX=/root/jonhy/toolchains/arm-buildroot-linux-gnueabi_sdk-buildroot/bin/arm-buildroot-linux-gnueabi- \
    ./build-local.sh \
      --target linux \
      --defconfig configs/mini_defconfig \
      --link mostly-static \
      --type MinSizeRel \
      --toolchain cmake/toolchains/linux-armv5-gnueabi.cmake \
      --target-name linux-armv5-gnueabi-mostly-static-mini

选项:
  --build-dir DIR        构建目录，默认 build-local
  --config FILE          Kconfig 输出文件，默认 BUILD_DIR/.config
  --target NAME          Kconfig target: linux | darwin | windows，默认自动
  --defconfig FILE       defconfig 文件，默认按平台自动选择
  --link MODE            dynamic | mostly-static | static，默认 mostly-static
  --type TYPE            CMake build type，默认 Release
  --generator NAME       CMake generator，例如 Ninja 或 Unix Makefiles
  --toolchain FILE       CMake toolchain file，用于交叉编译
  --target-name NAME     打包目标名，默认按平台/架构/链接模式生成
  --tests / --no-tests   是否构建并运行测试，默认 no-tests
  --package / --no-package
                         是否打包，默认 package
  --help                 显示帮助

环境变量:
  CC / CFLAGS / LDFLAGS              传给 CMake/编译器
  CTUNNEL_CROSS_PREFIX               交叉工具链前缀
  CTUNNEL_USE_BUNDLED_MONOCYPHER     默认 ON
  CMAKE_EXTRA_ARGS                   追加给 cmake configure 的参数
  VERSION                            打包版本，默认 0.1.0-dev+<git短hash>
EOF
}

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
cd "$repo_root"

build_dir=build-local
config_file=
kconfig_target=
defconfig=
link_mode=mostly-static
build_type=Release
generator=
toolchain=
target_name=
build_tests=OFF
package=1

while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir) build_dir=${2:?missing value for --build-dir}; shift 2 ;;
    --config) config_file=${2:?missing value for --config}; shift 2 ;;
    --target) kconfig_target=${2:?missing value for --target}; shift 2 ;;
    --defconfig) defconfig=${2:?missing value for --defconfig}; shift 2 ;;
    --link) link_mode=${2:?missing value for --link}; shift 2 ;;
    --type) build_type=${2:?missing value for --type}; shift 2 ;;
    --generator) generator=${2:?missing value for --generator}; shift 2 ;;
    --toolchain) toolchain=${2:?missing value for --toolchain}; shift 2 ;;
    --target-name) target_name=${2:?missing value for --target-name}; shift 2 ;;
    --tests) build_tests=ON; shift ;;
    --no-tests) build_tests=OFF; shift ;;
    --package) package=1; shift ;;
    --no-package) package=0; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "未知参数: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$link_mode" in
  dynamic) link_symbol=CTUNNEL_LINK_DYNAMIC ;;
  mostly-static) link_symbol=CTUNNEL_LINK_MOSTLY_STATIC ;;
  static) link_symbol=CTUNNEL_LINK_STATIC ;;
  *) echo "未知链接模式: $link_mode" >&2; exit 2 ;;
esac

host_os=$(uname -s)
host_arch=$(uname -m)
if [ -z "$kconfig_target" ]; then
  case "$host_os" in
    Darwin) kconfig_target=darwin ;;
    Linux) kconfig_target=linux ;;
    MINGW*|MSYS*|CYGWIN*) kconfig_target=windows ;;
    *) echo "无法自动判断平台，请指定 --target linux|darwin|windows" >&2; exit 2 ;;
  esac
fi

if [ -z "$defconfig" ]; then
  case "$kconfig_target" in
    darwin) defconfig=configs/macos_defconfig ;;
    windows) defconfig=configs/windows_defconfig ;;
    linux) defconfig=configs/default_defconfig ;;
    *) echo "未知 Kconfig target: $kconfig_target" >&2; exit 2 ;;
  esac
fi

if [ -z "$config_file" ]; then
  config_file="$build_dir/.config"
fi

if [ -z "$target_name" ]; then
  case "$kconfig_target" in
    darwin) target_os=macos ;;
    windows) target_os=windows ;;
    linux) target_os=linux ;;
    *) target_os=$kconfig_target ;;
  esac
  target_name="$target_os-$host_arch-$link_mode"
fi

mkdir -p "$build_dir"

echo "==> 生成配置"
echo "    target:    $kconfig_target"
echo "    defconfig: $defconfig"
echo "    link:      $link_mode"
echo "    build dir: $build_dir"
python3 scripts/kconfig/configure.py \
  --config "$config_file" \
  --build-dir "$build_dir" \
  --target "$kconfig_target" \
  defconfig "$defconfig"

scripts/config \
  --file "$config_file" \
  --build-dir "$build_dir" \
  --target "$kconfig_target" \
  --enable "$link_symbol"

cmake_args=(
  -S .
  -B "$build_dir"
  -DCMAKE_BUILD_TYPE="$build_type"
  -DCTUNNEL_KCONFIG_CONFIG="$config_file"
  -DCTUNNEL_USE_BUNDLED_MONOCYPHER="${CTUNNEL_USE_BUNDLED_MONOCYPHER:-ON}"
  -DCTUNNEL_BUILD_TESTS="$build_tests"
)

if [ -n "$generator" ]; then
  cmake_args+=(-G "$generator")
fi
if [ -n "$toolchain" ]; then
  cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$toolchain")
fi
if [ -n "${CMAKE_EXTRA_ARGS:-}" ]; then
  # shellcheck disable=SC2206
  extra_args=( ${CMAKE_EXTRA_ARGS} )
  cmake_args+=("${extra_args[@]}")
fi

echo "==> CMake 配置"
cmake "${cmake_args[@]}"

echo "==> 编译"
cmake --build "$build_dir" --parallel

binary="$build_dir/ctunnel"
if [ "$kconfig_target" = windows ]; then
  binary="$build_dir/ctunnel.exe"
fi

echo "==> 产物信息"
file "$binary" || true
if command -v otool >/dev/null 2>&1 && [ "$kconfig_target" = darwin ]; then
  otool -L "$binary" || true
elif command -v readelf >/dev/null 2>&1; then
  readelf -d "$binary" || true
fi

if [ "$build_tests" = ON ]; then
  echo "==> 运行测试"
  ctest --test-dir "$build_dir" --output-on-failure
fi

if [ "$package" -eq 1 ]; then
  if [ -z "${VERSION:-}" ]; then
    git_short=$(git rev-parse --short=12 HEAD 2>/dev/null || echo local)
    VERSION="0.1.0-dev+$git_short"
  fi
  echo "==> 打包"
  CTUNNEL_STRIP="${CTUNNEL_STRIP:-strip}" \
    VERSION="$VERSION" \
    TARGET="$target_name" \
    BINARY="$binary" \
    CONFIG="$config_file" \
    LINK_REPORT="$binary.link-report.txt" \
    scripts/package.sh
fi

echo "==> 完成"
echo "    binary:  $binary"
if [ "$package" -eq 1 ]; then
  case "$binary" in
    *.exe) echo "    package: ctunnel-$VERSION-$target_name.zip" ;;
    *) echo "    package: ctunnel-$VERSION-$target_name.tar.gz" ;;
  esac
fi
