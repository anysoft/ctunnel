#!/bin/sh
set -eu
target=${1:?usage: scripts/cross-build.sh TARGET TOOLCHAIN_FILE [dynamic|mostly-static|static]}
toolchain=${2:?usage: scripts/cross-build.sh TARGET TOOLCHAIN_FILE [dynamic|mostly-static|static]}
link_mode=${3:-mostly-static}
build="build-$target"
case "$target" in
  *windows*|*mingw*) kconfig_target=windows ;;
  *darwin*|*macos*) kconfig_target=darwin ;;
  *) kconfig_target=linux ;;
esac
python3 scripts/kconfig/configure.py --config "$build/.config" --build-dir "$build" \
  --target "$kconfig_target" defconfig configs/mini_defconfig
case "$link_mode" in
  dynamic) link_symbol=CTUNNEL_LINK_DYNAMIC ;;
  mostly-static) link_symbol=CTUNNEL_LINK_MOSTLY_STATIC ;;
  static) link_symbol=CTUNNEL_LINK_STATIC ;;
  *) echo "unknown link mode: $link_mode" >&2; exit 2 ;;
esac
scripts/config --file "$build/.config" --build-dir "$build" --target "$kconfig_target" \
  --enable "$link_symbol"
if [ "$kconfig_target" = windows ]; then
  scripts/config --file "$build/.config" --build-dir "$build" --target windows \
    --enable EVENT_WSAPOLL
fi
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DCTUNNEL_KCONFIG_CONFIG="$build/.config" -DCTUNNEL_USE_BUNDLED_MONOCYPHER=ON \
  -DCTUNNEL_BUILD_TESTS=OFF
cmake --build "$build" --parallel
file "$build/ctunnel"* || true
CTUNNEL_STRIP=${CTUNNEL_STRIP:-strip} scripts/size-report.sh "$build/ctunnel"*
