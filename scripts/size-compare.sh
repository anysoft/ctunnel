#!/bin/sh
set -eu

root=${CTUNNEL_SIZE_BUILD_ROOT:-build/size-comparison}
mkdir -p "$root"
echo "profile comparison"
printf '%-18s %-15s %14s %14s %14s\n' variant link_mode unstripped_bytes stripped_bytes delta_from_default
baseline=
for entry in default:configs/default_defconfig:Release mini-server:configs/mini_defconfig:MinSizeRel mini-client:configs/mini_client_defconfig:MinSizeRel full:configs/full_defconfig:Debug; do
  name=${entry%%:*}
  rest=${entry#*:}
  defconfig=${rest%%:*}
  build_type=${rest##*:}
  directory="$root/$name"
  config="$directory/.config"
  python3 scripts/kconfig/configure.py --config "$config" --build-dir "$directory" \
    defconfig "$defconfig" >/dev/null
  cmake -S . -B "$directory" -DCMAKE_BUILD_TYPE="$build_type" \
    -DCTUNNEL_KCONFIG_CONFIG="$config" -DCTUNNEL_BUILD_TESTS=OFF >/dev/null
  cmake --build "$directory" --parallel >/dev/null
  binary="$directory/ctunnel"
  copy=$(mktemp "${TMPDIR:-/tmp}/ctunnel-size.XXXXXX")
  cp "$binary" "$copy"
  if [ "$(uname -s)" = Darwin ]; then
    strip -x "$copy" 2>/dev/null || strip "$copy"
  else
    strip --strip-unneeded "$copy" 2>/dev/null || strip "$copy"
  fi
  unstripped=$(wc -c <"$binary" | tr -d ' ')
  stripped=$(wc -c <"$copy" | tr -d ' ')
  rm -f "$copy"
  if [ -z "$baseline" ]; then baseline=$stripped; fi
  delta=$((stripped - baseline))
  link_mode=$(sed -n 's/^link_mode: //p' "$binary.link-report.txt" | head -n 1)
  printf '%-18s %-15s %14s %14s %+14s\n' "$name" "$link_mode" "$unstripped" "$stripped" "$delta"
  CTUNNEL_STRIP=${CTUNNEL_STRIP:-strip} scripts/size-report.sh "$binary" >"$directory/size-report.txt"
  cp "$config" "$directory/ctunnel.config"
done

echo
echo "isolated feature comparison (Default limits/applets, MinSizeRel+LTO)"
printf '%-18s %-15s %14s %14s %14s\n' variant link_mode unstripped_bytes stripped_bytes delta_from_baseline
baseline=
for name in baseline role-server role-client keygen-off log-warn log-debug log-trace data-off work-pool-off build-info-off; do
  directory="$root/isolated-$name"
  config="$directory/.config"
  python3 scripts/kconfig/configure.py --config "$config" --build-dir "$directory" \
    defconfig configs/default_defconfig >/dev/null
  set -- --enable OPTIMIZE_FOR_SIZE --enable ENABLE_LTO
  case "$name" in
    role-server) set -- "$@" --enable CTUNNEL_SERVER --disable CTUNNEL_CLIENT ;;
    role-client) set -- "$@" --enable CTUNNEL_CLIENT --disable CTUNNEL_SERVER ;;
    keygen-off) set -- "$@" --disable FEATURE_KEYGEN ;;
    log-warn) set -- "$@" --enable LOG_MAX_LEVEL_WARN ;;
    log-debug) set -- "$@" --enable LOG_MAX_LEVEL_DEBUG ;;
    log-trace) set -- "$@" --enable LOG_MAX_LEVEL_TRACE ;;
    data-off) set -- "$@" --disable FEATURE_DATA_ENCRYPTION ;;
    work-pool-off) set -- "$@" --disable FEATURE_WORK_POOL ;;
    build-info-off) set -- "$@" --disable FEATURE_BUILD_INFO ;;
  esac
  scripts/config --file "$config" --build-dir "$directory" "$@" >/dev/null
  cmake -S . -B "$directory" -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCTUNNEL_KCONFIG_CONFIG="$config" -DCTUNNEL_BUILD_TESTS=OFF >/dev/null
  cmake --build "$directory" --parallel >/dev/null
  binary="$directory/ctunnel"
  copy=$(mktemp "${TMPDIR:-/tmp}/ctunnel-size.XXXXXX")
  cp "$binary" "$copy"
  if [ "$(uname -s)" = Darwin ]; then
    strip -x "$copy" 2>/dev/null || strip "$copy"
  else
    strip --strip-unneeded "$copy" 2>/dev/null || strip "$copy"
  fi
  unstripped=$(wc -c <"$binary" | tr -d ' ')
  stripped=$(wc -c <"$copy" | tr -d ' ')
  rm -f "$copy"
  if [ -z "$baseline" ]; then baseline=$stripped; fi
  delta=$((stripped - baseline))
  link_mode=$(sed -n 's/^link_mode: //p' "$binary.link-report.txt" | head -n 1)
  printf '%-18s %-15s %14s %14s %+14s\n' "$name" "$link_mode" "$unstripped" "$stripped" "$delta"
  CTUNNEL_STRIP=${CTUNNEL_STRIP:-strip} scripts/size-report.sh "$binary" >"$directory/size-report.txt"
  cp "$config" "$directory/ctunnel.config"
done
