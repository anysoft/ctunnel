#!/bin/sh
set -eu
version=${VERSION:-0.1.0-dev}
target=${TARGET:?set TARGET}
binary=${BINARY:-build/ctunnel}
format=${PACKAGE_FORMAT:-auto}
strip_tool=${CTUNNEL_STRIP:-}
output_dir=$(pwd)
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1"
  else
    shasum -a 256 "$1"
  fi
}

case "$version" in
  *[!A-Za-z0-9._+-]* | "" )
    echo "package: unsafe VERSION: $version" >&2
    exit 2
    ;;
esac
case "$target" in
  *[!A-Za-z0-9._+-]* | "" )
    echo "package: unsafe TARGET: $target" >&2
    exit 2
    ;;
esac
if [ ! -f "$binary" ]; then
  echo "package: binary not found: $binary" >&2
  exit 2
fi

stage=$(mktemp -d "${TMPDIR:-/tmp}/ctunnel-pkg.XXXXXX")
trap 'rm -rf "$stage"' EXIT INT TERM
mkdir -p "$stage/ctunnel-$version-$target/examples"
cp "$binary" "$repo_root/README.md" "$repo_root/LICENSE" "$stage/ctunnel-$version-$target/"
packaged_binary=$stage/ctunnel-$version-$target/$(basename "$binary")
if [ -n "$strip_tool" ] && command -v "$strip_tool" >/dev/null 2>&1; then
  if [ "$(uname -s)" = Darwin ]; then
    "$strip_tool" -x "$packaged_binary" 2>/dev/null || "$strip_tool" "$packaged_binary" || true
  else
    "$strip_tool" --strip-unneeded "$packaged_binary" 2>/dev/null || "$strip_tool" "$packaged_binary" || true
  fi
fi
cp "$repo_root/third_party/monocypher/LICENSE" "$stage/ctunnel-$version-$target/MONOCYPHER-LICENSE"
cp "$repo_root/examples/server.ini" "$repo_root/examples/client.ini" "$repo_root/examples/clients.ini" "$stage/ctunnel-$version-$target/examples/"
config=${CONFIG:-.config}
if [ -f "$config" ]; then
  cp "$config" "$stage/ctunnel-$version-$target/ctunnel.config"
fi
link_report=${LINK_REPORT:-$binary.link-report.txt}
if [ -f "$link_report" ]; then
  cp "$link_report" "$stage/ctunnel-$version-$target/ctunnel.link-report.txt"
fi
if [ "$format" = auto ]; then
  case "$binary" in
    *.exe) format=zip ;;
    *) format=tar.gz ;;
  esac
fi
case "$format" in
  zip)
    if ! command -v zip >/dev/null 2>&1; then
      echo "package: zip is required for Windows packages" >&2
      exit 2
    fi
    (cd "$stage" && zip -qr "$output_dir/ctunnel-$version-$target.zip" "ctunnel-$version-$target")
    sha256_file "ctunnel-$version-$target.zip"
    ;;
  tar.gz)
    tar -C "$stage" -czf "ctunnel-$version-$target.tar.gz" "ctunnel-$version-$target"
    sha256_file "ctunnel-$version-$target.tar.gz"
    ;;
  *)
    echo "package: unsupported PACKAGE_FORMAT: $format" >&2
    exit 2
    ;;
esac
